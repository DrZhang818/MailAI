#include <array>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "mailai/store.h"
#include "mailai/sync_types.h"
#include "store_test_support.h"

namespace {
using namespace mailai;
using namespace store_test;
constexpr std::array modes{OpenMode::read_only, OpenMode::read_write_create};

void seed(const std::filesystem::path& path) {
    expect(SqliteStore::open(path, OpenMode::read_write_create).has_value(),
        "test database should initialize");
    Database db(path);
    insert_account(db);
}

void expect_state(const Result<SyncState>& result,
    std::optional<std::uint32_t> validity,
    std::uint32_t cursor,
    std::optional<std::int64_t> attempted,
    std::optional<std::int64_t> succeeded) {
    if (!result)
        throw std::runtime_error(
            "state query failed at " + result.error().stage + ": " + result.error().message);
    expect(result->uid_validity == validity && result->last_discovered_uid == cursor &&
               result->last_attempt_at == attempted && result->last_success_at == succeeded,
        "sync state fields should preserve their exact values and NULLs");
}

void expect_error(const auto& result,
    ErrorCode code,
    std::string_view account,
    std::string_view stage = "storage.state") {
    expect(!result && result.error().code == code, "query should return the expected error code");
    expect(result.error().account_id == std::optional<AccountId>{std::string{account}} &&
               !result.error().mail_id && result.error().stage == stage,
        "query errors should identify the account and operation");
}

void test_unsynced_account_and_argument_errors() {
    TempDirectory directory;
    const auto path = directory.path() / "unsynced.sqlite3";
    seed(path);
    Database db(path);
    const auto catalog = db.catalog();
    const auto accounts = db.rows("SELECT * FROM accounts ORDER BY id");
    for (auto mode : modes) {
        auto store = SqliteStore::open(path, mode);
        expect(store.has_value(), "unsynced database should reopen");
        expect_state(store->state("school", "INBOX"), std::nullopt, 0, std::nullopt, std::nullopt);
        expect_error(store->state("missing", "INBOX"), ErrorCode::unknown_account, "missing");
        expect_error(store->state("school' OR 1=1 --", "INBOX"),
            ErrorCode::unknown_account,
            "school' OR 1=1 --");
        expect_error(store->state("", "INBOX"), ErrorCode::invalid_argument, "");
        expect_error(store->state("school", ""), ErrorCode::invalid_argument, "school");
        expect_state(store->state("school", "INBOX"), std::nullopt, 0, std::nullopt, std::nullopt);
    }
    expect(db.integer("SELECT count(*) FROM sync_state") == 0,
        "reading an unsynced account must not create state");
    expect(db.catalog() == catalog && db.rows("SELECT * FROM accounts ORDER BY id") == accounts,
        "successful and failed state queries must preserve schema and account records");
}

void test_saved_state_filtering_and_integer_boundaries() {
    TempDirectory directory;
    const auto path = directory.path() / "saved.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO accounts SELECT 'other',display_name,address,imap_host,imap_port,
            imap_username,credential_env,tls_mode FROM accounts WHERE id='school';
        INSERT INTO sync_state VALUES
            ('school','INBOX',100,42,0,-10),
            ('school','Archive',200,7,NULL,0),
            ('other','INBOX',4294967295,4294967295,-9223372036854775808,9223372036854775807),
            ('other','Empty',NULL,0,NULL,NULL);
    )sql");
    const auto saved = db.rows("SELECT * FROM sync_state ORDER BY account_id,folder");
    const auto catalog = db.catalog();
    for (auto mode : modes) {
        std::optional<SyncState> owned;
        {
            auto store = SqliteStore::open(path, mode);
            expect(store.has_value(), "saved state database should reopen");
            expect_state(store->state("school", "INBOX"), 100, 42, 0, -10);
            expect_state(store->state("school", "Archive"), 200, 7, std::nullopt, 0);
            const auto largest = store->state("other", "INBOX");
            expect_state(largest,
                4294967295U,
                4294967295U,
                std::numeric_limits<std::int64_t>::min(),
                std::numeric_limits<std::int64_t>::max());
            owned = *largest;
            expect_state(
                store->state("other", "Empty"), std::nullopt, 0, std::nullopt, std::nullopt);
            expect_state(
                store->state("school", "Missing"), std::nullopt, 0, std::nullopt, std::nullopt);
            expect_state(store->state("school", "INBOX' OR 1=1 --"),
                std::nullopt,
                0,
                std::nullopt,
                std::nullopt);
        }
        expect(owned->uid_validity == 4294967295U && owned->last_discovered_uid == 4294967295U &&
                   owned->last_attempt_at == std::numeric_limits<std::int64_t>::min() &&
                   owned->last_success_at == std::numeric_limits<std::int64_t>::max(),
            "returned state should survive closing the connection");
    }
    expect(db.catalog() == catalog &&
               db.rows("SELECT * FROM sync_state ORDER BY account_id,folder") == saved,
        "state queries must preserve existing records and schema");
}

void test_invalid_stored_fields_and_recovery() {
    TempDirectory directory;
    const auto path = directory.path() / "invalid.sqlite3";
    seed(path);
    Database db(path);
    db.execute("PRAGMA ignore_check_constraints=ON");
    db.execute("INSERT INTO sync_state VALUES ('school','INBOX',100,42,NULL,NULL)");
    auto store = SqliteStore::open(path, OpenMode::read_only);
    expect(store.has_value(), "invalid-value database should open with intact schema");
    constexpr std::array mutations{
        "uid_validity=0",
        "uid_validity=4294967296",
        "uid_validity=1.5",
        "uid_validity='broken'",
        "uid_validity=NULL,last_discovered_uid=1",
        "last_discovered_uid=-1",
        "last_discovered_uid=4294967296",
        "last_discovered_uid=1.5",
        "last_discovered_uid='broken'",
        "last_attempt_at='broken'",
        "last_attempt_at=1.5",
        "last_success_at='broken'",
        "last_success_at=1.5"
    };
    for (const auto* mutation : mutations) {
        db.execute("UPDATE sync_state SET " + std::string{mutation});
        const auto damaged = db.rows("SELECT * FROM sync_state");
        expect_error(store->state("school", "INBOX"), ErrorCode::storage_error, "school");
        expect(db.rows("SELECT * FROM sync_state") == damaged,
            "failed reads must not repair or overwrite stored values");
        db.execute(
            "UPDATE sync_state SET uid_validity=100,last_discovered_uid=42,"
            "last_attempt_at=NULL,last_success_at=NULL");
        expect_state(store->state("school", "INBOX"), 100, 42, std::nullopt, std::nullopt);
    }
}

void test_sql_failure_and_recovery() {
    TempDirectory directory;
    const auto path = directory.path() / "recovery.sqlite3";
    seed(path);
    Database db(path);
    const auto ddl = db.text("SELECT sql FROM sqlite_schema WHERE name='sync_state'");
    auto store = SqliteStore::open(path, OpenMode::read_only);
    expect(store.has_value(), "recovery database should open");
    db.execute("DROP TABLE sync_state");
    expect_error(store->state("school", "INBOX"), ErrorCode::storage_error, "school");
    expect_error(store->state("school", "INBOX"), ErrorCode::storage_error, "school");
    db.execute(ddl);
    expect_state(store->state("school", "INBOX"), std::nullopt, 0, std::nullopt, std::nullopt);
}

void expect_pending(const auto& result,
    std::string_view account,
    std::string_view folder,
    std::uint32_t validity,
    std::initializer_list<std::uint32_t> uids) {
    if (!result)
        throw std::runtime_error(
            "pending query failed at " + result.error().stage + ": " + result.error().message);
    expect(result->size() == uids.size(), "only the expected pending jobs should be returned");
    std::size_t index = 0;
    for (const auto uid : uids) {
        const auto& key = (*result)[index++];
        expect(key.account_id == account && key.folder == folder && key.uid_validity == validity &&
                   key.uid == uid,
            "pending keys should retain identity and follow ascending UID order");
    }
}

template <class Store>
void test_pending_empty_account_and_errors() {
    TempDirectory directory;
    const auto path = directory.path() / "pending-empty.sqlite3";
    seed(path);
    Database db(path);
    const auto catalog = db.catalog();
    const auto accounts = db.rows("SELECT * FROM accounts ORDER BY id");
    for (auto mode : modes) {
        auto store = Store::open(path, mode);
        expect(store.has_value(), "pending database should open");
        expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {});
        expect_error(store->pending("missing", "INBOX"),
            ErrorCode::unknown_account,
            "missing",
            "storage.pending");
        expect_error(store->pending("school' OR 1=1 --", "INBOX"),
            ErrorCode::unknown_account,
            "school' OR 1=1 --",
            "storage.pending");
        expect_error(
            store->pending("", "INBOX"), ErrorCode::invalid_argument, "", "storage.pending");
        expect_error(
            store->pending("school", ""), ErrorCode::invalid_argument, "school", "storage.pending");
        expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {});
    }
    expect(db.integer("SELECT count(*) FROM sync_state") == 0 &&
               db.integer("SELECT count(*) FROM sync_pending") == 0,
        "reading pending jobs must not create records");
    expect(db.catalog() == catalog && db.rows("SELECT * FROM accounts ORDER BY id") == accounts,
        "pending queries must preserve schema and account records");
}

template <class Store>
void test_pending_filtering_order_and_persistence() {
    TempDirectory directory;
    const auto path = directory.path() / "pending-filter.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO accounts SELECT 'other',display_name,address,imap_host,imap_port,
            imap_username,credential_env,tls_mode FROM accounts WHERE id='school';
        INSERT INTO sync_state VALUES
            ('school','INBOX',100,44,0,-10),
            ('school','Archive',200,7,NULL,NULL),
            ('school','Empty',100,0,NULL,NULL),
            ('school','NoRange',NULL,0,NULL,NULL),
            ('other','INBOX',4294967295,4294967295,NULL,NULL);
        INSERT INTO sync_pending(account_id,folder,uid_validity,uid) VALUES
            ('school','INBOX',100,44), ('school','INBOX',100,41),
            ('school','INBOX',100,43), ('school','INBOX',100,42),
            ('school','INBOX',99,1), ('school','Archive',200,2),
            ('school','NoRange',100,9), ('other','INBOX',4294967295,4294967295);
        UPDATE sync_pending SET last_error_code='network_error',
            last_error_stage='imap.fetch',last_error_message='retry me'
            WHERE account_id='school' AND folder='INBOX' AND uid=41;
        UPDATE sync_pending SET retryable=0,last_error_code='remote_gone',
            last_error_stage='imap.fetch',last_error_message='gone'
            WHERE account_id='school' AND folder='INBOX' AND uid=42;
    )sql");
    const auto states = db.rows("SELECT * FROM sync_state ORDER BY account_id,folder");
    const auto jobs =
        db.rows("SELECT * FROM sync_pending ORDER BY account_id,folder,uid_validity,uid");
    const auto catalog = db.catalog();
    for (auto mode : modes) {
        Result<std::vector<RemoteKey>> owned;
        {
            auto store = Store::open(path, mode);
            expect(store.has_value(), "pending jobs should survive reopening the database");
            AccountId account = "school";
            std::string folder = "INBOX";
            owned = store->pending(account, folder);
            expect_pending(owned, "school", "INBOX", 100, {41, 43, 44});
            account.assign("changed");
            folder.assign("changed");
            expect_pending(store->pending("school", "Archive"), "school", "Archive", 200, {2});
            expect_pending(store->pending("school", "Missing"), "school", "Missing", 100, {});
            expect_pending(store->pending("school", "Empty"), "school", "Empty", 100, {});
            expect_pending(store->pending("school", "NoRange"), "school", "NoRange", 100, {});
            expect_pending(store->pending("school", "INBOX' OR 1=1 --"),
                "school",
                "INBOX' OR 1=1 --",
                100,
                {});
            expect_pending(
                store->pending("other", "INBOX"), "other", "INBOX", 4294967295U, {4294967295U});
        }
        expect_pending(owned, "school", "INBOX", 100, {41, 43, 44});
    }
    expect(db.catalog() == catalog &&
               db.rows("SELECT * FROM sync_state ORDER BY account_id,folder") == states &&
               db.rows("SELECT * FROM sync_pending ORDER BY account_id,folder,uid_validity,uid") ==
                   jobs,
        "pending reads must preserve progress, retry flags, error diagnostics and schema");
}

template <class Store>
void test_pending_owned_keys_with_embedded_nulls() {
    TempDirectory directory;
    const auto path = directory.path() / "pending-bytes.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO accounts SELECT CAST(X'7363686F6F6C007461696C' AS TEXT),
            display_name,address,imap_host,imap_port,imap_username,credential_env,tls_mode
            FROM accounts WHERE id='school';
        INSERT INTO sync_state VALUES (CAST(X'7363686F6F6C007461696C' AS TEXT),
            CAST(X'494E424F58007461696C' AS TEXT),100,41,NULL,NULL);
        INSERT INTO sync_pending(account_id,folder,uid_validity,uid)
            VALUES (CAST(X'7363686F6F6C007461696C' AS TEXT),
                CAST(X'494E424F58007461696C' AS TEXT),100,41);
    )sql");
    Result<std::vector<RemoteKey>> owned;
    {
        auto store = Store::open(path, OpenMode::read_only);
        expect(store.has_value(), "byte-key database should open");
        AccountId account{"school\0tail", 11};
        std::string folder{"INBOX\0tail", 10};
        owned = store->pending(account, folder);
        expect_pending(owned, account, folder, 100, {41});
        account.assign("changed");
        folder.assign("changed");
    }
    expect_pending(
        owned, std::string{"school\0tail", 11}, std::string{"INBOX\0tail", 10}, 100, {41});
}

template <class Store>
void test_pending_invalid_fields_and_recovery() {
    TempDirectory directory;
    const auto path = directory.path() / "pending-invalid.sqlite3";
    seed(path);
    Database db(path);
    db.execute("PRAGMA ignore_check_constraints=ON");
    db.execute("INSERT INTO sync_state VALUES ('school','INBOX',100,44,NULL,NULL)");
    db.execute(
        "INSERT INTO sync_pending(account_id,folder,uid_validity,uid) VALUES "
        "('school','INBOX',100,41),('school','INBOX',100,43)");
    auto store = Store::open(path, OpenMode::read_only);
    expect(store.has_value(), "invalid pending database should open");
    constexpr std::array bad_uids{"0", "-1", "4294967296", "1.5", "'broken'"};
    for (const auto* value : bad_uids) {
        db.execute("UPDATE sync_pending SET uid=" + std::string{value} + " WHERE uid=43");
        const auto damaged = db.rows("SELECT * FROM sync_pending ORDER BY uid");
        expect_error(store->pending("school", "INBOX"),
            ErrorCode::storage_error,
            "school",
            "storage.pending");
        expect(db.rows("SELECT * FROM sync_pending ORDER BY uid") == damaged,
            "failed pending reads must not overwrite damaged values");
        db.execute("DELETE FROM sync_pending WHERE uid<>41");
        db.execute(
            "INSERT INTO sync_pending(account_id,folder,uid_validity,uid) "
            "VALUES ('school','INBOX',100,43)");
        expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {41, 43});
    }
    constexpr std::array bad_validities{"0", "-1", "4294967296", "1.5", "'broken'"};
    for (const auto* value : bad_validities) {
        db.execute("UPDATE sync_state SET uid_validity=" + std::string{value});
        db.execute("UPDATE sync_pending SET uid_validity=" + std::string{value});
        expect_error(store->pending("school", "INBOX"),
            ErrorCode::storage_error,
            "school",
            "storage.pending");
        db.execute("UPDATE sync_state SET uid_validity=100");
        db.execute("UPDATE sync_pending SET uid_validity=100");
        expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {41, 43});
    }
}

template <class Store>
void test_pending_sql_failure_and_recovery() {
    TempDirectory directory;
    const auto path = directory.path() / "pending-recovery.sqlite3";
    seed(path);
    Database db(path);
    const auto ddl = db.text("SELECT sql FROM sqlite_schema WHERE name='sync_pending'");
    auto store = Store::open(path, OpenMode::read_only);
    expect(store.has_value(), "pending recovery database should open");
    db.execute("DROP TABLE sync_pending");
    expect_error(
        store->pending("school", "INBOX"), ErrorCode::storage_error, "school", "storage.pending");
    expect_error(
        store->pending("school", "INBOX"), ErrorCode::storage_error, "school", "storage.pending");
    db.execute(ddl);
    expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {});
}

template <class Store>
void test_schedule_rejects_invalid_input_without_writing() {
    TempDirectory directory;
    const auto path = directory.path() / "schedule-input.sqlite3";
    seed(path);
    Database db(path);
    auto store = Store::open(path, OpenMode::read_write_create);
    expect(store.has_value(), "schedule database should open");

    const MailboxSnapshot snapshot{100, {3, 1, 2}};
    expect_error(store->schedule("", "INBOX", snapshot, 20),
        ErrorCode::invalid_argument,
        "",
        "storage.schedule");
    expect_error(store->schedule("school", "", snapshot, 20),
        ErrorCode::invalid_argument,
        "school",
        "storage.schedule");
    expect_error(store->schedule("school", "INBOX", MailboxSnapshot{}, 20),
        ErrorCode::invalid_argument,
        "school",
        "storage.schedule");
    expect_error(store->schedule("school", "INBOX", MailboxSnapshot{100, {1, 0, 2}}, 20),
        ErrorCode::invalid_argument,
        "school",
        "storage.schedule");
    expect_error(store->schedule("school", "INBOX", snapshot, 0),
        ErrorCode::invalid_argument,
        "school",
        "storage.schedule");
    expect_error(store->schedule("missing", "INBOX", snapshot, 20),
        ErrorCode::unknown_account,
        "missing",
        "storage.schedule");
    expect(db.integer("SELECT count(*) FROM sync_state") == 0 &&
               db.integer("SELECT count(*) FROM sync_pending") == 0,
        "rejected scheduling must not create progress or jobs");
}

template <class Store>
void test_schedule_selects_latest_unique_uids_without_mutating_snapshot() {
    TempDirectory directory;
    const auto path = directory.path() / "schedule-first.sqlite3";
    seed(path);
    const MailboxSnapshot snapshot{100, {9, 3, 7, 5, 9, 1}};
    {
        auto store = Store::open(path, OpenMode::read_write_create);
        expect(store.has_value(), "initial schedule database should open");
        expect(store->schedule("school", "INBOX", snapshot, 3).has_value(),
            "first schedule should save the latest unique jobs");
        expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {5, 7, 9});
        expect_state(store->state("school", "INBOX"), 100, 9, std::nullopt, std::nullopt);
    }
    expect(snapshot.uids == std::vector<std::uint32_t>({9, 3, 7, 5, 9, 1}),
        "scheduling must not reorder the caller's snapshot");
    auto reopened = Store::open(path, OpenMode::read_only);
    expect(reopened.has_value(), "scheduled work should survive reopening");
    expect_pending(reopened->pending("school", "INBOX"), "school", "INBOX", 100, {5, 7, 9});
}

template <class Store>
void test_schedule_preserves_failed_jobs_and_never_moves_cursor_backwards() {
    TempDirectory directory;
    const auto path = directory.path() / "schedule-resume.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO sync_state VALUES ('school','INBOX',100,10,0,-10);
        INSERT INTO sync_pending VALUES
            ('school','INBOX',100,3,1,'network_error','imap.fetch','retry later'),
            ('school','INBOX',100,8,0,'remote_gone','imap.fetch','gone');
    )sql");
    const auto old_jobs = db.rows("SELECT * FROM sync_pending ORDER BY uid");
    auto store = Store::open(path, OpenMode::read_write_create);
    expect(store.has_value(), "incremental schedule database should open");
    const MailboxSnapshot snapshot{100, {12, 8, 3, 11, 12}};
    expect(store->schedule("school", "INBOX", snapshot, 1).has_value(),
        "later schedules must include all new UIDs rather than reapply the first-run limit");
    expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {3, 11, 12});
    expect_state(store->state("school", "INBOX"), 100, 12, 0, -10);
    expect(db.rows("SELECT * FROM sync_pending WHERE uid<=10 ORDER BY uid") == old_jobs,
        "discovery must preserve existing retry flags and diagnostics");
    const auto saved = db.rows("SELECT * FROM sync_pending ORDER BY uid");
    expect(store->schedule("school", "INBOX", snapshot, 1).has_value(),
        "repeating discovery should succeed");
    expect(store->schedule("school", "INBOX", MailboxSnapshot{100, {1, 2}}, 1).has_value(),
        "a snapshot with a smaller maximum UID should not reset progress");
    expect(store->schedule("school", "INBOX", MailboxSnapshot{100, {}}, 1).has_value(),
        "an empty later snapshot should not erase unfinished work");
    expect_state(store->state("school", "INBOX"), 100, 12, 0, -10);
    expect(db.rows("SELECT * FROM sync_pending ORDER BY uid") == saved,
        "repeated or older discovery must not replace jobs or erase failures");
}

template <class Store>
void test_schedule_empty_first_snapshot_does_not_limit_later_mail() {
    TempDirectory directory;
    const auto path = directory.path() / "schedule-empty.sqlite3";
    seed(path);
    auto store = Store::open(path, OpenMode::read_write_create);
    expect(store.has_value(), "empty schedule database should open");
    expect(store->schedule("school", "INBOX", MailboxSnapshot{100, {}}, 2).has_value(),
        "an empty first snapshot should establish its UIDVALIDITY");
    expect_state(store->state("school", "INBOX"), 100, 0, std::nullopt, std::nullopt);
    expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {});
    expect(store->schedule("school", "INBOX", MailboxSnapshot{100, {4, 2, 3, 1}}, 2).has_value(),
        "all subsequently arriving mail should be scheduled");
    expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {1, 2, 3, 4});
}

template <class Store>
void test_schedule_skips_successfully_saved_mail_but_keeps_parse_failures() {
    TempDirectory directory;
    const auto path = directory.path() / "schedule-saved.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO messages
            (id,account_id,folder,uid_validity,uid,sender,subject,body,
             received_at_utc,raw,parse_state,parse_error)
        VALUES
            (101,'school','INBOX',100,3,'sender','saved','body',10,X'0001','parsed',''),
            (102,'school','INBOX',100,4,'sender','(解析失败)','',11,X'0002','failed','bad MIME');
    )sql");
    const auto saved = db.rows("SELECT * FROM messages ORDER BY id");
    auto store = Store::open(path, OpenMode::read_write_create);
    expect(store.has_value(), "saved-mail schedule database should open");
    expect(store->schedule("school", "INBOX", MailboxSnapshot{100, {3, 4, 5}}, 3).has_value(),
        "scheduling should recognize previously saved messages");
    expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 100, {4, 5});
    expect(db.rows("SELECT * FROM messages ORDER BY id") == saved,
        "discovery must not replace existing message content or IDs");
}

template <class Store>
void test_schedule_new_uidvalidity_preserves_history_and_other_folders() {
    TempDirectory directory;
    const auto path = directory.path() / "schedule-range.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO accounts SELECT 'other',display_name,address,imap_host,imap_port,
            imap_username,credential_env,tls_mode FROM accounts WHERE id='school';
        INSERT INTO sync_state VALUES
            ('school','INBOX',100,10,0,-10),
            ('school','Archive',100,55,NULL,NULL),
            ('other','INBOX',100,12,NULL,NULL);
        INSERT INTO sync_pending(account_id,folder,uid_validity,uid) VALUES
            ('school','INBOX',100,3),('school','Archive',100,55),('other','INBOX',100,12);
        INSERT INTO messages
            (id,account_id,folder,uid_validity,uid,sender,subject,body,received_at_utc,raw,parse_state)
        VALUES
            (1,'school','INBOX',100,10,'sender','old','body',10,X'0001','parsed'),
            (2,'school','Archive',100,10,'sender','archive','body',10,X'0002','parsed'),
            (3,'other','INBOX',100,10,'sender','other','body',10,X'0003','parsed');
        INSERT INTO attachments VALUES (1,0,'file.txt','text/plain',2);
    )sql");
    const auto attachments = db.rows("SELECT * FROM attachments");
    auto store = Store::open(path, OpenMode::read_write_create);
    expect(store.has_value(), "changed-range database should open");
    expect(store->schedule("school", "INBOX", MailboxSnapshot{200, {2, 1, 3}}, 2).has_value(),
        "a new UIDVALIDITY should schedule a new initial range");
    expect_state(store->state("school", "INBOX"), 200, 3, 0, -10);
    expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 200, {2, 3});
    expect(db.integer("SELECT count(*) FROM sync_pending WHERE account_id='school' "
                      "AND folder='INBOX' AND uid_validity=100") == 0,
        "old-range jobs should be removed");
    expect(db.rows("SELECT id,historical FROM messages ORDER BY id") ==
               std::vector<std::vector<std::string>>({{"1", "1"}, {"2", "0"}, {"3", "0"}}),
        "only old messages in the changed account and folder should become historical");
    expect(db.rows("SELECT * FROM attachments") == attachments,
        "changing UIDVALIDITY must preserve message attachments");
    expect_pending(store->pending("school", "Archive"), "school", "Archive", 100, {55});
    expect_pending(store->pending("other", "INBOX"), "other", "INBOX", 100, {12});
}

template <class Store>
void test_schedule_rolls_back_progress_jobs_and_history_after_insert_failure() {
    TempDirectory directory;
    const auto path = directory.path() / "schedule-rollback.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO sync_state VALUES ('school','INBOX',100,7,0,-10);
        INSERT INTO sync_pending(account_id,folder,uid_validity,uid) VALUES ('school','INBOX',100,1);
        INSERT INTO messages
            (id,account_id,folder,uid_validity,uid,sender,subject,body,received_at_utc,raw,parse_state)
        VALUES (42,'school','INBOX',100,7,'sender','old','body',10,X'0001','parsed');
    )sql");
    auto store = Store::open(path, OpenMode::read_write_create);
    expect(store.has_value(), "rollback schedule database should open");
    db.execute(R"sql(
        CREATE TRIGGER reject_second_job BEFORE INSERT ON sync_pending
        WHEN NEW.uid=2 BEGIN SELECT RAISE(ABORT,'injected schedule failure'); END;
    )sql");
    const auto states = db.rows("SELECT * FROM sync_state");
    const auto jobs = db.rows("SELECT * FROM sync_pending");
    const auto messages = db.rows("SELECT * FROM messages");
    const MailboxSnapshot snapshot{200, {1, 2, 3}};
    expect_error(store->schedule("school", "INBOX", snapshot, 3),
        ErrorCode::storage_error,
        "school",
        "storage.schedule");
    expect(db.rows("SELECT * FROM sync_state") == states &&
               db.rows("SELECT * FROM sync_pending") == jobs &&
               db.rows("SELECT * FROM messages") == messages,
        "an insertion failure must roll back new progress, jobs and historical flags together");
    db.execute("DROP TRIGGER reject_second_job");
    expect(store->schedule("school", "INBOX", snapshot, 3).has_value(),
        "the same connection should schedule successfully after removing the fault");
    expect_pending(store->pending("school", "INBOX"), "school", "INBOX", 200, {1, 2, 3});
    expect_state(store->state("school", "INBOX"), 200, 3, 0, -10);
    expect(db.integer("SELECT historical FROM messages WHERE id=42") == 1,
        "history should change only after a successful transaction");
}

template <class Store>
void test_schedule_read_only_failure_preserves_records() {
    TempDirectory directory;
    const auto path = directory.path() / "schedule-readonly.sqlite3";
    seed(path);
    Database db(path);
    auto store = Store::open(path, OpenMode::read_only);
    expect(store.has_value(), "read-only schedule database should open");
    const auto result = store->schedule("school", "INBOX", MailboxSnapshot{100, {1, 2}}, 20);
    expect(!result && result.error().code == ErrorCode::storage_error &&
               result.error().account_id == std::optional<AccountId>{"school"},
        "scheduling through a read-only connection must report an account-scoped error");
    expect(db.integer("SELECT count(*) FROM sync_state") == 0 &&
               db.integer("SELECT count(*) FROM sync_pending") == 0,
        "a read-only scheduling attempt must not create progress or jobs");
}

template <class Store>
void test_schedule_preserves_full_keys_and_uint32_max() {
    TempDirectory directory;
    const auto path = directory.path() / "schedule-bytes.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO accounts SELECT CAST(X'7363686F6F6C007461696C' AS TEXT),
            display_name,address,imap_host,imap_port,imap_username,credential_env,tls_mode
            FROM accounts WHERE id='school';
    )sql");
    auto store = Store::open(path, OpenMode::read_write_create);
    expect(store.has_value(), "full-key schedule database should open");
    const AccountId account{"school\0tail", 11};
    const std::string folder{"INBOX\0tail", 10};
    const MailboxSnapshot snapshot{4294967295U, {4294967295U, 1}};
    expect(store->schedule(account, folder, snapshot, std::numeric_limits<std::size_t>::max())
               .has_value(),
        "scheduling must preserve full keys and unsigned UID boundaries");
    expect_pending(store->pending(account, folder), account, folder, 4294967295U, {1, 4294967295U});
    expect_state(
        store->state(account, folder), 4294967295U, 4294967295U, std::nullopt, std::nullopt);
    expect_state(store->state("school", "INBOX"), std::nullopt, 0, std::nullopt, std::nullopt);
}

void run_schedule_tests() {
    test_schedule_rejects_invalid_input_without_writing<SqliteStore>();
    test_schedule_selects_latest_unique_uids_without_mutating_snapshot<SqliteStore>();
    test_schedule_preserves_failed_jobs_and_never_moves_cursor_backwards<SqliteStore>();
    test_schedule_empty_first_snapshot_does_not_limit_later_mail<SqliteStore>();
    test_schedule_skips_successfully_saved_mail_but_keeps_parse_failures<SqliteStore>();
    test_schedule_new_uidvalidity_preserves_history_and_other_folders<SqliteStore>();
    test_schedule_rolls_back_progress_jobs_and_history_after_insert_failure<SqliteStore>();
    test_schedule_read_only_failure_preserves_records<SqliteStore>();
    test_schedule_preserves_full_keys_and_uint32_max<SqliteStore>();
}

void run_pending_tests() {
    test_pending_empty_account_and_errors<SqliteStore>();
    test_pending_filtering_order_and_persistence<SqliteStore>();
    test_pending_owned_keys_with_embedded_nulls<SqliteStore>();
    test_pending_invalid_fields_and_recovery<SqliteStore>();
    test_pending_sql_failure_and_recovery<SqliteStore>();
}

}  // namespace

int main() {
    try {
        test_unsynced_account_and_argument_errors();
        test_saved_state_filtering_and_integer_boundaries();
        test_invalid_stored_fields_and_recovery();
        test_sql_failure_and_recovery();
        run_pending_tests();
        run_schedule_tests();
        std::cout << "store_sync tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "mailai/mime.h"
#include "mailai/store.h"
#include "store_test_support.h"

namespace {
using namespace mailai;
using namespace store_test;

void seed(const std::filesystem::path& path) {
    expect(SqliteStore::open(path, OpenMode::read_write_create).has_value(),
        "write-test database should initialize");
    Database db(path);
    insert_account(db);
}

void expect_success(const auto& result, const char* message) {
    if (!result)
        throw std::runtime_error(
            std::string{message} + ": " + result.error().stage + ": " + result.error().message);
}

void expect_error(const auto& result,
    ErrorCode code,
    std::string_view stage,
    std::string_view account = "school") {
    expect(!result && result.error().code == code, "write should report the expected error");
    expect(result.error().stage == stage &&
               result.error().account_id == std::optional<AccountId>{std::string{account}} &&
               !result.error().mail_id,
        "write errors should identify their account and operation");
}

FetchedMail fetched(std::uint32_t uid = 1) {
    return {{"school", "INBOX", 100, uid}, -10, std::string{"raw\0bytes", 9}};
}

ParsedMail parsed_mail() {
    ParsedMail result;
    result.sender = "发件人 <sender@example.invalid>";
    result.subject = "课程通知";
    result.body = "正文";
    result.body.push_back('\0');
    result.body += "结尾";
    result.message_id = "<one@example.invalid>";
    result.in_reply_to = "<parent@example.invalid>";
    result.references = "<first@example.invalid> <parent@example.invalid>";
    result.attachments = {{"简历.pdf", "application/pdf", 12}, {"notes.txt", "text/plain", 0}};
    return result;
}

template <class Store>
void prepare_jobs(Store& store, std::vector<std::uint32_t> uids = {1, 2, 3}) {
    expect_success(store.schedule("school", "INBOX", MailboxSnapshot{100, std::move(uids)}, 20),
        "mailbox jobs should be scheduled");
}

template <class Store>
void test_complete_preserves_bytes_ids_and_replaces_attachments() {
    TempDirectory directory;
    const auto path = directory.path() / "complete-roundtrip.sqlite3";
    seed(path);
    Database db(path);
    MailId id{};
    const auto mail = fetched();
    auto parsed = parsed_mail();
    {
        auto store = Store::open(path, OpenMode::read_write_create);
        expect_success(store, "write store should open");
        prepare_jobs(*store);
        auto saved = store->complete(mail, parsed);
        expect_success(saved, "parsed mail should save");
        id = *saved;
        expect(id > 0, "saved mail should receive a positive local ID");
        auto found = store->find(id);
        expect_success(found, "saved mail should be readable");
        expect(found->subject == parsed.subject && found->body == parsed.body &&
                   found->sender == parsed.sender && found->message_id == parsed.message_id &&
                   found->in_reply_to == parsed.in_reply_to &&
                   found->references == parsed.references && found->received_at_utc == -10 &&
                   found->account_id == "school" && found->folder == "INBOX" &&
                   found->uid_validity == 100 && found->uid == 1 &&
                   found->parse_state == ParseState::parsed && found->parse_error.empty() &&
                   !found->historical,
            "saved mail should preserve its values and remote identity");
        expect(found->attachments.size() == 2 && found->attachments[0].name == "简历.pdf" &&
                   found->attachments[0].size_bytes == 12 &&
                   found->attachments[1].name == "notes.txt" &&
                   found->attachments[1].size_bytes == 0,
            "attachments should retain their values and order");
        expect(db.text("SELECT typeof(raw) FROM messages") == "blob" &&
                   db.text("SELECT hex(raw) FROM messages") == "726177006279746573",
            "raw bytes including embedded NUL must be stored as a BLOB");
        const auto pending = store->pending("school", "INBOX");
        expect_success(pending, "remaining jobs should be readable");
        expect(pending->size() == 2 && (*pending)[0].uid == 2 && (*pending)[1].uid == 3,
            "saving one mail should remove only its own pending job");

        parsed.subject = "更新后的主题";
        parsed.attachments = {{"replacement.txt", "text/plain", 7}};
        const auto saved_again = store->complete(mail, parsed);
        expect_success(saved_again, "saving a remote message again should succeed");
        expect(*saved_again == id && db.integer("SELECT count(*) FROM messages") == 1 &&
                   db.integer("SELECT count(*) FROM attachments") == 1,
            "retrying must preserve the local ID and replace the old attachment list");
        parsed.attachments.clear();
        const auto saved_without_attachments = store->complete(mail, parsed);
        expect_success(
            saved_without_attachments, "saving a message without attachments should succeed");
        expect(
            *saved_without_attachments == id && db.integer("SELECT count(*) FROM attachments") == 0,
            "an empty replacement must remove obsolete attachments");
    }
    auto reopened = Store::open(path, OpenMode::read_only);
    expect_success(reopened, "saved database should reopen read-only");
    auto found = reopened->find(id);
    expect_success(found, "saved mail should survive reopening");
    expect(found->subject == "更新后的主题" && found->body == parsed.body &&
               found->attachments.empty(),
        "reopening must retain the message content and final attachment list");
}

template <class Store>
void test_complete_parse_failure_remains_retryable_and_can_recover() {
    TempDirectory directory;
    const auto path = directory.path() / "complete-parse.sqlite3";
    seed(path);
    Database db(path);
    auto store = Store::open(path, OpenMode::read_write_create);
    expect_success(store, "parse-failure store should open");
    prepare_jobs(*store, {1});
    const auto mail = fetched();
    const Result<ParsedMail> failure = std::unexpected(Error{
        ErrorCode::parse_error, "unsupported charset", std::nullopt, std::nullopt, "mime.charset"
    });
    auto saved = store->complete(mail, failure);
    expect_success(saved, "a parsing failure should still preserve the raw message");
    auto found = store->find(*saved);
    expect_success(found, "a failed message should remain readable");
    expect(found->parse_state == ParseState::failed && found->subject == "(解析失败)" &&
               found->body.empty() && found->parse_error == "unsupported charset" &&
               found->attachments.empty(),
        "a failed parse should have an explicit placeholder and diagnostic");
    expect(db.rows("SELECT retryable,last_error_code,last_error_stage,last_error_message "
                   "FROM sync_pending") ==
               std::vector<std::vector<std::string>>(
                   {{"1", "parse_error", "mime.charset", "unsupported charset"}}),
        "parsing failure should persist a retryable job and its original diagnostic");
    const auto recovered = store->complete(mail, parsed_mail());
    expect_success(recovered, "a later successful parse should save");
    expect(*recovered == *saved, "parse recovery must retain the local ID");
    found = store->find(*saved);
    expect_success(found, "recovered message should be readable");
    expect(found->parse_state == ParseState::parsed && found->parse_error.empty() &&
               found->attachments.size() == 2 &&
               db.integer("SELECT count(*) FROM sync_pending") == 0,
        "successful recovery should replace the placeholder and remove the job");
    auto no_subject = parsed_mail();
    no_subject.subject.clear();
    expect_success(store->complete(mail, no_subject), "a missing subject should be supported");
    found = store->find(*saved);
    expect_success(found, "mail without an original subject should be readable");
    expect(found->subject == "(无主题)", "successful mail without a subject needs its fallback");
}

template <class Store>
void test_complete_rejects_invalid_input_without_changing_saved_data() {
    TempDirectory directory;
    const auto path = directory.path() / "complete-invalid.sqlite3";
    seed(path);
    Database db(path);
    auto store = Store::open(path, OpenMode::read_write_create);
    expect_success(store, "invalid-input store should open");
    prepare_jobs(*store);
    expect_success(store->complete(fetched(), parsed_mail()), "baseline mail should save");
    const auto messages = db.rows("SELECT * FROM messages");
    const auto attachments = db.rows("SELECT * FROM attachments");
    const auto jobs = db.rows("SELECT * FROM sync_pending ORDER BY uid");
    for (const auto& key : std::array<RemoteKey, 4>{
             {{"", "INBOX", 100, 1},
                 {"school", "", 100, 1},
                 {"school", "INBOX", 0, 1},
                 {"school", "INBOX", 100, 0}}
         }) {
        auto mail = fetched();
        mail.key = key;
        expect_error(store->complete(mail, parsed_mail()),
            ErrorCode::invalid_argument,
            "storage.complete",
            key.account_id);
    }
    auto oversized = parsed_mail();
    oversized.attachments[0].size_bytes = std::numeric_limits<std::uint64_t>::max();
    expect_error(
        store->complete(fetched(), oversized), ErrorCode::invalid_argument, "storage.complete");
    const Result<ParsedMail> wrong_error = std::unexpected(Error{
        ErrorCode::network_error, "not a parse result", std::nullopt, std::nullopt, "imap.fetch"
    });
    expect_error(
        store->complete(fetched(), wrong_error), ErrorCode::invalid_argument, "storage.complete");
    const Result<ParsedMail> empty_error = std::unexpected(
        Error{ErrorCode::parse_error, "", std::nullopt, std::nullopt, "mime.parse"});
    expect_error(
        store->complete(fetched(), empty_error), ErrorCode::invalid_argument, "storage.complete");
    auto unknown = fetched();
    unknown.key.account_id = "missing";
    expect_error(store->complete(unknown, parsed_mail()),
        ErrorCode::unknown_account,
        "storage.complete",
        "missing");
    expect(db.rows("SELECT * FROM messages") == messages &&
               db.rows("SELECT * FROM attachments") == attachments &&
               db.rows("SELECT * FROM sync_pending ORDER BY uid") == jobs,
        "invalid input must not alter mail, attachments or pending jobs");
}

template <class Store>
void test_complete_rolls_back_all_changes_when_attachment_write_fails() {
    TempDirectory directory;
    const auto path = directory.path() / "complete-rollback.sqlite3";
    seed(path);
    Database db(path);
    auto store = Store::open(path, OpenMode::read_write_create);
    expect_success(store, "rollback store should open");
    prepare_jobs(*store, {1});
    auto original = parsed_mail();
    const auto saved = store->complete(fetched(), original);
    expect_success(saved, "baseline mail should save");
    db.execute(
        "INSERT INTO sync_pending(account_id,folder,uid_validity,uid) "
        "VALUES ('school','INBOX',100,1)");
    db.execute(R"sql(
        CREATE TRIGGER reject_attachment BEFORE INSERT ON attachments
        WHEN NEW.ordinal=1 BEGIN SELECT RAISE(ABORT,'injected attachment failure'); END;
    )sql");
    const auto messages = db.rows("SELECT * FROM messages");
    const auto attachments = db.rows("SELECT * FROM attachments ORDER BY ordinal");
    const auto jobs = db.rows("SELECT * FROM sync_pending");
    auto replacement = parsed_mail();
    replacement.subject = "replacement";
    replacement.attachments[0].name = "new.txt";
    auto raw = fetched();
    raw.raw = "replacement raw";
    expect_error(store->complete(raw, replacement), ErrorCode::storage_error, "storage.complete");
    expect(db.rows("SELECT * FROM messages") == messages &&
               db.rows("SELECT * FROM attachments ORDER BY ordinal") == attachments &&
               db.rows("SELECT * FROM sync_pending") == jobs,
        "an attachment failure must roll back the mail update, attachment replacement and job "
        "removal");
    db.execute("DROP TRIGGER reject_attachment");
    const auto retried = store->complete(raw, replacement);
    expect_success(retried, "the same connection should recover after removing the fault");
    expect(*retried == *saved && db.integer("SELECT count(*) FROM sync_pending") == 0,
        "successful retry must preserve the ID and finish the job");
}

template <class Store>
void test_remote_identity_scopes_and_stale_results() {
    TempDirectory directory;
    const auto path = directory.path() / "complete-scopes.sqlite3";
    seed(path);
    Database db(path);
    db.execute(
        "INSERT INTO accounts SELECT 'other',display_name,address,imap_host,imap_port,"
        "imap_username,credential_env,tls_mode FROM accounts WHERE id='school'");
    auto store = Store::open(path, OpenMode::read_write_create);
    expect_success(store, "scope store should open");
    prepare_jobs(*store, {1});
    const auto inbox = store->complete(fetched(), parsed_mail());
    expect_success(inbox, "first inbox mail should save");
    expect_success(store->schedule("school", "Archive", MailboxSnapshot{100, {1}}, 20),
        "another folder should schedule");
    auto mail = fetched();
    mail.key.folder = "Archive";
    const auto archive = store->complete(mail, parsed_mail());
    expect_success(archive, "same UID in another folder should save separately");
    expect_success(store->schedule("other", "INBOX", MailboxSnapshot{100, {1}}, 20),
        "another account should schedule");
    mail.key = {"other", "INBOX", 100, 1};
    const auto other = store->complete(mail, parsed_mail());
    expect_success(other, "same UID in another account should save separately");
    expect_success(store->schedule("school", "INBOX", MailboxSnapshot{200, {1}}, 20),
        "a new server range should schedule");
    const auto before_stale = db.rows("SELECT * FROM messages ORDER BY id");
    expect_error(
        store->complete(fetched(), parsed_mail()), ErrorCode::invalid_argument, "storage.complete");
    const Error error{
        ErrorCode::network_error, "late failure", std::nullopt, std::nullopt, "imap.fetch"
    };
    expect_error(store->fail(fetched().key, error), ErrorCode::invalid_argument, "storage.fail");
    expect(db.rows("SELECT * FROM messages ORDER BY id") == before_stale,
        "stale results must not overwrite historical messages");
    mail = fetched();
    mail.key.uid_validity = 200;
    const auto new_range = store->complete(mail, parsed_mail());
    expect_success(new_range, "the current range should save separately");
    expect(*inbox != *archive && *inbox != *other && *inbox != *new_range && *archive != *other &&
               *archive != *new_range && *other != *new_range,
        "all four remote identities need distinct local IDs");
    const auto historical = store->find(*inbox);
    expect_success(historical, "historical message should remain readable");
    expect(historical->historical && db.integer("SELECT count(*) FROM messages") == 4,
        "old mail should be retained as history");
}

template <class Store>
void test_fail_records_errors_without_reactivating_removed_remote_mail() {
    TempDirectory directory;
    const auto path = directory.path() / "fail-errors.sqlite3";
    seed(path);
    Database db(path);
    auto store = Store::open(path, OpenMode::read_write_create);
    expect_success(store, "failure-recording store should open");
    prepare_jobs(*store, {1, 2});
    const auto state_before = db.rows("SELECT * FROM sync_state");
    const Error network{
        ErrorCode::network_error, "temporary failure", std::nullopt, std::nullopt, "imap.fetch"
    };
    expect_success(store->fail(fetched().key, network), "temporary failure should be recorded");
    expect(db.rows("SELECT retryable,last_error_code,last_error_stage,last_error_message "
                   "FROM sync_pending WHERE uid=1") ==
               std::vector<std::vector<std::string>>(
                   {{"1", "network_error", "imap.fetch", "temporary failure"}}),
        "a transient failure should retain retryability and its diagnostic");
    const Error gone{
        ErrorCode::remote_gone, "message no longer exists", std::nullopt, std::nullopt, "imap.fetch"
    };
    expect_success(store->fail(fetched().key, gone), "remote disappearance should be recorded");
    expect_success(
        store->fail(fetched().key, network), "a repeated failure report should be supported");
    const auto pending = store->pending("school", "INBOX");
    expect_success(pending, "remaining retryable jobs should be readable");
    expect(pending->size() == 1 && (*pending)[0].uid == 2 &&
               db.integer("SELECT retryable FROM sync_pending WHERE uid=1") == 0,
        "later error reports must not reactivate a task already made nonretryable");
    expect(db.rows("SELECT * FROM sync_state") == state_before &&
               db.integer("SELECT count(*) FROM messages") == 0,
        "recording a fetch failure must not advance discovery or invent mail");
    expect_success(store->complete(fetched(2), parsed_mail()), "another job should complete");
    expect_error(store->fail(fetched(2).key, network), ErrorCode::invalid_argument, "storage.fail");
    const auto unchanged = db.rows("SELECT * FROM sync_pending");
    const Error invalid{
        static_cast<ErrorCode>(999), "invalid code", std::nullopt, std::nullopt, "test"
    };
    expect_error(store->fail(fetched().key, invalid), ErrorCode::invalid_argument, "storage.fail");
    const Error empty{ErrorCode::network_error, "", std::nullopt, std::nullopt, "imap.fetch"};
    expect_error(store->fail(fetched().key, empty), ErrorCode::invalid_argument, "storage.fail");
    expect(db.rows("SELECT * FROM sync_pending") == unchanged,
        "invalid failure reports must leave existing diagnostics unchanged");
}

template <class Store>
void test_late_parse_failure_does_not_reactivate_removed_remote_mail() {
    TempDirectory directory;
    const auto path = directory.path() / "late-parse.sqlite3";
    seed(path);
    Database db(path);
    auto store = Store::open(path, OpenMode::read_write_create);
    expect_success(store, "late-result store should open");
    prepare_jobs(*store, {1});
    const auto mail = fetched();
    const Error gone{
        ErrorCode::remote_gone, "message no longer exists", std::nullopt, std::nullopt, "imap.fetch"
    };
    expect_success(store->fail(mail.key, gone), "remote disappearance should stop retrying");
    const Result<ParsedMail> failure = std::unexpected(Error{
        ErrorCode::parse_error, "unsupported charset", std::nullopt, std::nullopt, "mime.charset"
    });
    const auto saved = store->complete(mail, failure);
    expect_success(saved, "a late parsing result should still preserve downloaded bytes");
    const auto pending = store->pending("school", "INBOX");
    expect_success(pending, "pending jobs should remain readable");
    expect(pending->empty() && db.integer("SELECT retryable FROM sync_pending") == 0,
        "a late parsing failure must not reactivate a remotely removed message");
    const auto found = store->find(*saved);
    expect_success(found, "the downloaded message should remain readable");
    expect(found->parse_state == ParseState::failed && found->parse_error == "unsupported charset",
        "stopping retries must not discard an already downloaded message");
}

template <class Store>
void test_fail_uses_stable_error_names_and_rolls_back_on_sql_error() {
    TempDirectory directory;
    const auto path = directory.path() / "fail-names.sqlite3";
    seed(path);
    Database db(path);
    auto store = Store::open(path, OpenMode::read_write_create);
    expect_success(store, "error-name store should open");
    prepare_jobs(*store, {1});
    const std::pair<ErrorCode, const char*> codes[]{
        {ErrorCode::unknown_account, "unknown_account"},
        {ErrorCode::mail_not_found, "mail_not_found"},
        {ErrorCode::invalid_argument, "invalid_argument"},
        {ErrorCode::config_error, "config_error"},
        {ErrorCode::auth_error, "auth_error"},
        {ErrorCode::network_error, "network_error"},
        {ErrorCode::protocol_error, "protocol_error"},
        {ErrorCode::remote_gone, "remote_gone"},
        {ErrorCode::parse_error, "parse_error"},
        {ErrorCode::storage_error, "storage_error"},
        {ErrorCode::busy, "busy"}
    };
    for (const auto& [code, name] : codes) {
        const Error error{code, "controlled failure", std::nullopt, std::nullopt, "test.fetch"};
        expect_success(store->fail(fetched().key, error), "known error code should save");
        expect(db.text("SELECT last_error_code FROM sync_pending") == name,
            "persisted error names must be readable strings rather than enum ordinals");
    }
    db.execute(R"sql(
        CREATE TRIGGER reject_failure BEFORE UPDATE ON sync_pending
        BEGIN SELECT RAISE(ABORT,'injected failure-recording error'); END;
    )sql");
    const auto before = db.rows("SELECT * FROM sync_pending");
    const Error network{
        ErrorCode::network_error, "new reason", std::nullopt, std::nullopt, "imap.fetch"
    };
    expect_error(store->fail(fetched().key, network), ErrorCode::storage_error, "storage.fail");
    expect(db.rows("SELECT * FROM sync_pending") == before,
        "a failed UPDATE must retain old diagnostics");
    db.execute("DROP TRIGGER reject_failure");
    expect_success(store->fail(fetched().key, network), "failure recording should recover");
}

template <class Store>
void test_record_outcome_preserves_cursor_and_last_success_on_failure() {
    TempDirectory directory;
    const auto path = directory.path() / "outcome.sqlite3";
    seed(path);
    Database db(path);
    auto store = Store::open(path, OpenMode::read_write_create);
    expect_success(store, "outcome store should open");
    const Result<SyncSummary> failed = std::unexpected(
        Error{ErrorCode::auth_error, "authentication failed", "school", std::nullopt, "imap.auth"});
    expect_success(store->record_outcome("school", "INBOX", failed, 0),
        "a failed first run should still record its attempt time");
    auto state = store->state("school", "INBOX");
    expect_success(state, "first failed attempt should be readable");
    expect(!state->uid_validity && state->last_discovered_uid == 0 && state->last_attempt_at == 0 &&
               !state->last_success_at,
        "failure before discovery must not invent a server range or successful run");
    prepare_jobs(*store, {1, 2});
    const auto jobs = db.rows("SELECT * FROM sync_pending ORDER BY uid");
    SyncSummary partial;
    partial.success = 1;
    partial.failed = 1;
    expect_success(store->record_outcome("school", "INBOX", partial, -10),
        "a partially successful run should record its attempt");
    state = store->state("school", "INBOX");
    expect_success(state, "partial outcome should be readable");
    expect(state->uid_validity == 100 && state->last_discovered_uid == 2 &&
               state->last_attempt_at == -10 && !state->last_success_at,
        "partial failure must preserve discovery without claiming full success");
    const Result<SyncSummary> success = SyncSummary{};
    expect_success(store->record_outcome("school", "INBOX", success, 10),
        "a successful empty run should save");
    expect_success(
        store->record_outcome("school", "INBOX", failed, 20), "a later failed run should save");
    state = store->state("school", "INBOX");
    expect_success(state, "last success should remain readable");
    expect(state->last_attempt_at == 20 && state->last_success_at == 10,
        "failed runs must not overwrite the last successful time");
    expect_success(
        store->record_outcome("school", "INBOX", success, std::numeric_limits<std::int64_t>::min()),
        "negative timestamp boundary should save");
    state = store->state("school", "INBOX");
    expect_success(state, "timestamp boundary should be readable");
    expect(state->last_success_at == std::numeric_limits<std::int64_t>::min(),
        "outcome timestamps must preserve the full signed range");
    expect(db.rows("SELECT * FROM sync_pending ORDER BY uid") == jobs,
        "recording an outcome must not alter pending work");
    expect_error(store->record_outcome("missing", "INBOX", success, 30),
        ErrorCode::unknown_account,
        "storage.outcome",
        "missing");
    expect_error(store->record_outcome("school", "", success, 30),
        ErrorCode::invalid_argument,
        "storage.outcome");
}

template <class Store>
void test_write_methods_preserve_full_keys_and_read_only_data() {
    TempDirectory directory;
    const auto path = directory.path() / "write-bytes.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO accounts SELECT CAST(X'7363686F6F6C007461696C' AS TEXT),
            display_name,address,imap_host,imap_port,imap_username,credential_env,tls_mode
            FROM accounts WHERE id='school';
    )sql");
    const AccountId account{"school\0tail", 11};
    const std::string folder{"INBOX\0tail", 10};
    auto mail = fetched();
    mail.key = {account, folder, 4294967295U, 4294967295U};
    MailId id{};
    {
        auto store = Store::open(path, OpenMode::read_write_create);
        expect_success(store, "full-key write store should open");
        expect_success(
            store->schedule(account, folder, MailboxSnapshot{4294967295U, {4294967295U}}, 20),
            "full-length remote key should schedule");
        const Error network{
            ErrorCode::network_error, "retry", std::nullopt, std::nullopt, "imap.fetch"
        };
        expect_success(store->fail(mail.key, network), "failure should match the full-length key");
        const auto saved = store->complete(mail, parsed_mail());
        expect_success(saved, "mail should save under a full-length key");
        id = *saved;
        expect_success(store->record_outcome(account, folder, SyncSummary{}, 0),
            "outcome should match the full-length key");
    }
    auto store = Store::open(path, OpenMode::read_only);
    expect_success(store, "full-key database should reopen read-only");
    const auto found = store->find(id);
    expect_success(found, "full-key mail should be readable");
    expect(found->account_id == account && found->folder == folder &&
               found->uid_validity == 4294967295U && found->uid == 4294967295U,
        "mail writes must not truncate keys or wrap unsigned server identifiers");
    const auto before = db.rows("SELECT * FROM messages");
    const auto denied = store->complete(mail, parsed_mail());
    expect(!denied && denied.error().code == ErrorCode::storage_error,
        "read-only connections must reject mail writes");
    const auto outcome_denied = store->record_outcome(account, folder, SyncSummary{}, 1);
    expect(!outcome_denied && outcome_denied.error().code == ErrorCode::storage_error,
        "read-only connections must reject outcome writes");
    expect(db.rows("SELECT * FROM messages") == before,
        "read-only write attempts must preserve saved mail");
}

void run_write_tests() {
    test_complete_preserves_bytes_ids_and_replaces_attachments<SqliteStore>();
    test_complete_parse_failure_remains_retryable_and_can_recover<SqliteStore>();
    test_complete_rejects_invalid_input_without_changing_saved_data<SqliteStore>();
    test_complete_rolls_back_all_changes_when_attachment_write_fails<SqliteStore>();
    test_remote_identity_scopes_and_stale_results<SqliteStore>();
    test_fail_records_errors_without_reactivating_removed_remote_mail<SqliteStore>();
    test_late_parse_failure_does_not_reactivate_removed_remote_mail<SqliteStore>();
    test_fail_uses_stable_error_names_and_rolls_back_on_sql_error<SqliteStore>();
    test_record_outcome_preserves_cursor_and_last_success_on_failure<SqliteStore>();
    test_write_methods_preserve_full_keys_and_read_only_data<SqliteStore>();
}
}  // namespace

int main() {
    try {
        run_write_tests();
        std::cout << "store_write tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

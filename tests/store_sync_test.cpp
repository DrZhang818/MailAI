#include <array>
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

void expect_error(const Result<SyncState>& result, ErrorCode code, std::string_view account) {
    expect(!result && result.error().code == code, "state should return the expected error code");
    expect(result.error().account_id == std::optional<AccountId>{std::string{account}} &&
               !result.error().mail_id && result.error().stage == "storage.state",
        "state errors should identify the account and operation");
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

}  // namespace

int main() {
    try {
        test_unsynced_account_and_argument_errors();
        test_saved_state_filtering_and_integer_boundaries();
        test_invalid_stored_fields_and_recovery();
        test_sql_failure_and_recovery();
        std::cout << "store_sync tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "mailai/store.h"

#include <array>
#include <chrono>
#include <iostream>
#include <limits>

#include "store/schema.h"
#include "store/sqlite_utils.h"
#include "store_test_support.h"

namespace {
using namespace mailai;
using namespace store_test;
constexpr std::array modes{OpenMode::read_only, OpenMode::read_write_create};

ImapSettings settings() {
    ImapSettings result;
    result.host = "imap.example.invalid";
    result.username = "student";
    result.credential_env = "TEST_PASSWORD_REF";
    return result;
}
void expect_storage_error(const Result<SqliteStore>& result) {
    expect(!result, "opening an incompatible database should fail");
    expect(result.error().code == ErrorCode::storage_error, "rejection should be a storage error");
    expect(!result.error().message.empty(), "rejection should contain a diagnostic");
}
void initialize(const std::filesystem::path& path) {
    expect(SqliteStore::open(path, OpenMode::read_write_create).has_value(),
           "version 1 should initialize");
}

void test_initialize_and_reopen() {
    TempDirectory directory;
    const auto path = directory.path() / "邮件.sqlite3";
    {
        auto store = SqliteStore::open(path, OpenMode::read_write_create);
        expect(store.has_value(), "a new database should initialize");
        expect(store->ensure_account({"school", "学校邮箱", "student@example.invalid"}, settings())
                   .has_value(),
               "new schema should support account writes");
    }
    Database db(path, SQLITE_OPEN_READONLY);
    expect(db.integer("PRAGMA user_version") == 1, "initialization should commit version 1");
    expect(
        db.rows(
            "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT GLOB 'sqlite_*' ORDER "
            "BY name") ==
            std::vector<std::vector<std::string>>{
                {"accounts"}, {"attachments"}, {"messages"}, {"sync_pending"}, {"sync_state"}},
        "all five tables should exist");
    expect(db.integer("SELECT count(*) FROM sqlite_schema WHERE type='index' AND name IN "
                      "('messages_inbox_order','messages_account_order')") == 2,
           "both inbox indexes should exist");
    const auto before = db.catalog();
    for (auto mode : modes)
        expect(SqliteStore::open(path, mode).has_value(),
               "valid version 1 should reopen in both modes");
    expect(db.catalog() == before && db.integer("SELECT count(*) FROM accounts") == 1,
           "reopen should preserve schema and accounts");
}

void test_original_version_one_reopens() {
    constexpr const char* original_accounts_sql = R"sql(CREATE TABLE accounts (
                id TEXT PRIMARY KEY NOT NULL,
                display_name TEXT NOT NULL,
                address TEXT NOT NULL,
                imap_host TEXT NOT NULL,
                imap_port INTEGER NOT NULL,
                imap_username TEXT NOT NULL,
                credential_env TEXT NOT NULL,
                tls_mode TEXT NOT NULL
            );)sql";
    TempDirectory directory;
    const auto path = directory.path() / "original-version-one.sqlite3";
    initialize(path);
    Database db(path);
    db.execute("DROP TABLE accounts");
    db.execute(original_accounts_sql);
    insert_account(db);
    const auto before = db.catalog();
    const auto accounts = db.rows("SELECT * FROM accounts");
    for (auto mode : modes) {
        expect(SqliteStore::open(path, mode).has_value(),
               "original version 1 should reopen in both modes");
        expect(db.catalog() == before && db.rows("SELECT * FROM accounts") == accounts &&
                   db.integer("PRAGMA user_version") == 1,
               "reopening must preserve the schema, account values and version");
    }
}

void test_empty_and_missing_paths() {
    TempDirectory directory;
    const auto path = directory.path() / "empty.sqlite3";
    expect_storage_error(SqliteStore::open(path, OpenMode::read_only));
    expect(!std::filesystem::exists(path), "read-only missing file must not be created");
    {
        Database empty(path);
        empty.execute("PRAGMA user_version=0");
    }
    expect_storage_error(SqliteStore::open(path, OpenMode::read_only));
    {
        Database empty(path, SQLITE_OPEN_READONLY);
        expect(empty.catalog().empty() && empty.integer("PRAGMA user_version") == 0,
               "read-only rejection must not initialize an empty database");
    }
    initialize(path);
    Database db(path);
    expect(db.integer("PRAGMA user_version") == 1, "empty file should initialize version 1");
    const auto missing = directory.path() / "missing" / "db.sqlite3";
    expect_storage_error(SqliteStore::open(missing, OpenMode::read_write_create));
    expect(!std::filesystem::exists(missing.parent_path()),
           "store must not create parent directories");
}

void test_unversioned_account_database_is_preserved() {
    TempDirectory directory;
    const auto source = directory.path() / "source.sqlite3";
    initialize(source);
    Database source_db(source, SQLITE_OPEN_READONLY);
    const auto accounts_sql = source_db.text("SELECT sql FROM sqlite_schema WHERE name='accounts'");
    const auto path = directory.path() / "unversioned-accounts.sqlite3";
    Database db(path);
    db.execute(accounts_sql);
    insert_account(db);
    const auto original = db.rows("SELECT * FROM accounts");
    const auto before = db.catalog();
    for (auto mode : modes) {
        expect_storage_error(SqliteStore::open(path, mode));
        expect(db.catalog() == before && db.integer("PRAGMA user_version") == 0,
               "nonempty unversioned database must retain its schema and version");
        expect(db.rows("SELECT * FROM accounts") == original,
               "rejected unversioned database must retain exact account values");
    }
}

void test_unsupported_versions() {
    for (int version : {-1, 2, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        for (auto mode : modes) {
            TempDirectory directory;
            const auto path = directory.path() / "version.sqlite3";
            Database db(path);
            db.execute(
                "CREATE TABLE sentinel(value TEXT); INSERT INTO sentinel VALUES ('keep me'); "
                "PRAGMA user_version=" +
                std::to_string(version));
            const auto before = db.catalog();
            expect_storage_error(SqliteStore::open(path, mode));
            expect(db.integer("PRAGMA user_version") == version && db.catalog() == before,
                   "version rejection should retain version and schema");
            expect(db.text("SELECT value FROM sentinel") == "keep me",
                   "version rejection must retain data");
        }
    }
}

void test_unversioned_schema_variants() {
    for (const auto* sql :
         {"CREATE TABLE sentinel(value TEXT); INSERT INTO sentinel VALUES('keep me');",
          "CREATE VIEW sentinel AS SELECT 'keep me' AS value;",
          "CREATE TABLE sentinel(value TEXT); INSERT INTO sentinel VALUES('keep me'); "
          "CREATE INDEX extra_index ON sentinel(value);",
          "CREATE TABLE sentinel(value TEXT); INSERT INTO sentinel VALUES('keep me'); "
          "CREATE TRIGGER refuse_delete BEFORE DELETE ON sentinel BEGIN SELECT "
          "RAISE(ABORT,'disabled'); END;"}) {
        for (auto mode : modes) {
            TempDirectory directory;
            const auto path = directory.path() / "unversioned.sqlite3";
            Database db(path);
            db.execute(sql);
            const auto before = db.catalog();
            expect_storage_error(SqliteStore::open(path, mode));
            expect(db.catalog() == before && db.integer("PRAGMA user_version") == 0,
                   "nonempty unversioned schema must not be initialized");
            expect(db.text("SELECT value FROM sentinel") == "keep me",
                   "unversioned rejection must preserve existing contents");
        }
    }
}

void test_unversioned_complete_database_is_preserved() {
    TempDirectory directory;
    const auto path = directory.path() / "unversioned-complete.sqlite3";
    initialize(path);
    Database db(path);
    insert_account(db);
    db.execute("PRAGMA user_version=0");
    const auto before = db.catalog();
    const auto accounts = db.rows("SELECT * FROM accounts");
    for (auto mode : modes) {
        expect_storage_error(SqliteStore::open(path, mode));
        expect(db.catalog() == before && db.integer("PRAGMA user_version") == 0,
               "complete but unversioned schema must not be adopted");
        expect(db.rows("SELECT * FROM accounts") == accounts,
               "unversioned rejection must retain account values");
    }
}

void test_version_one_without_schema_is_rejected() {
    TempDirectory directory;
    const auto path = directory.path() / "marked-empty.sqlite3";
    Database db(path);
    db.execute("PRAGMA user_version=1");
    for (auto mode : modes) {
        expect_storage_error(SqliteStore::open(path, mode));
        expect(db.catalog().empty() && db.integer("PRAGMA user_version") == 1,
               "empty version 1 must not be reinitialized");
    }
}

void test_version_one_definition_variants() {
    TempDirectory directory;
    const auto source = directory.path() / "source.sqlite3";
    initialize(source);
    Database source_db(source, SQLITE_OPEN_READONLY);
    const auto original = source_db.text("SELECT sql FROM sqlite_schema WHERE name='accounts'");
    const std::vector<std::string> variants{
        replace_once(original, "imap_port INTEGER NOT NULL", "imap_port TEXT NOT NULL"),
        replace_once(original, "id TEXT PRIMARY KEY NOT NULL", "id TEXT NOT NULL"),
        replace_once(original, "imap_host TEXT NOT NULL", "imap_host TEXT"),
        replace_once(original, "tls_mode TEXT NOT NULL",
                     "tls_mode TEXT NOT NULL, extra TEXT NOT NULL"),
        replace_once(original, "display_name TEXT NOT NULL",
                     "display_name TEXT NOT NULL DEFAULT 'extra'"),
        replace_once(original, "address TEXT NOT NULL", "address TEXT NOT NULL UNIQUE"),
        replace_once(original, "address TEXT NOT NULL", "address TEXT NOT NULL COLLATE NOCASE"),
        replace_once(original, "imap_port INTEGER NOT NULL",
                     "imap_port INTEGER NOT NULL CHECK(imap_port=993)"),
        replace_once(original, "tls_mode TEXT NOT NULL",
                     "tls_mode TEXT NOT NULL, extra TEXT GENERATED ALWAYS AS (id)")};
    for (const auto& sql : variants) {
        for (auto mode : modes) {
            TempDirectory variant_directory;
            const auto path = variant_directory.path() / "definition.sqlite3";
            initialize(path);
            Database db(path);
            db.execute("DROP TABLE accounts;");
            db.execute(sql);
            const auto before = db.catalog();
            expect_storage_error(SqliteStore::open(path, mode));
            expect(db.catalog() == before && db.integer("PRAGMA user_version") == 1,
                   "incompatible version 1 definition must not be repaired");
        }
    }
}

void test_version_one_catalog_variants() {
    for (const auto& mutation :
         {"DROP TABLE attachments;", "DROP INDEX messages_inbox_order;",
          "CREATE TABLE extra(value TEXT); INSERT INTO extra VALUES('keep me');",
          "CREATE VIEW account_names AS SELECT id FROM accounts;",
          "CREATE INDEX extra_index ON accounts(address);",
          "CREATE TRIGGER refuse_accounts BEFORE INSERT ON accounts BEGIN SELECT "
          "RAISE(ABORT,'disabled'); END;"}) {
        for (auto mode : modes) {
            TempDirectory directory;
            const auto path = directory.path() / "changed.sqlite3";
            initialize(path);
            Database db(path);
            insert_account(db);
            db.execute(mutation);
            const auto before = db.catalog();
            const auto accounts = db.rows("SELECT * FROM accounts");
            expect_storage_error(SqliteStore::open(path, mode));
            expect(db.catalog() == before && db.rows("SELECT * FROM accounts") == accounts,
                   "rejected version 1 must not be repaired or lose data");
            expect(db.integer("PRAGMA user_version") == 1, "rejected version 1 must retain marker");
        }
    }
}

void test_non_database_file() {
    TempDirectory directory;
    const auto path = directory.path() / "not-database";
    const std::string original = "not an SQLite database\n";
    {
        std::ofstream output(path, std::ios::binary);
        output << original;
    }
    for (auto mode : modes) {
        expect_storage_error(SqliteStore::open(path, mode));
        expect(read_file(path) == original, "non-database file bytes must be preserved");
    }
}

void test_account_save_and_identity_protection() {
    TempDirectory directory;
    const auto path = directory.path() / "account.sqlite3";
    auto store = SqliteStore::open(path, OpenMode::read_write_create);
    expect(store.has_value(), "account store should open");
    Account account{"school", "学校邮箱", "student@example.invalid"};
    const auto imap = settings();
    expect(store->ensure_account(account, imap).has_value(), "account should save");
    expect(store->ensure_account(account, imap).has_value(), "repeated account should save");
    account.display_name = "Student's 学校邮箱";
    expect(store->ensure_account(account, imap).has_value(), "display name should update");
    for (int change = 0; change < 3; ++change) {
        auto other = imap;
        if (change == 0)
            other.host = "other.example.invalid";
        if (change == 1)
            other.port = 143;
        if (change == 2)
            other.username = "another";
        auto result = store->ensure_account(account, other);
        expect(!result && result.error().code == ErrorCode::config_error,
               "identity changes should be rejected");
    }
    Database db(path, SQLITE_OPEN_READONLY);
    expect(db.integer("SELECT count(*) FROM accounts") == 1, "saving must not duplicate accounts");
    expect(db.text("SELECT display_name FROM accounts") == account.display_name,
           "display name should be preserved");
    expect(db.text("SELECT imap_host FROM accounts") == imap.host &&
               db.integer("SELECT imap_port FROM accounts") == 993,
           "rejected changes must retain identity");
    expect(db.text("SELECT credential_env FROM accounts") == "TEST_PASSWORD_REF",
           "only credential reference should persist");
    auto readonly = SqliteStore::open(path, OpenMode::read_only);
    expect(readonly.has_value(), "valid database should open read-only");
    expect(!readonly->ensure_account(account, imap), "read-only storage must reject writes");
}

struct Denial {
    int action;
    std::string first;
    std::string second;
    bool triggered{false};
};
int deny_operation(void* context, int action, const char* first, const char* second, const char*,
                   const char*) {
    auto& denial = *static_cast<Denial*>(context);
    if (action == denial.action && (denial.first.empty() || (first && first == denial.first)) &&
        (denial.second.empty() || (second && second == denial.second))) {
        denial.triggered = true;
        return SQLITE_DENY;
    }
    return SQLITE_OK;
}

void test_schema_failures_roll_back() {
    using namespace mailai::store_detail;
    for (const auto& failure : std::array{Denial{SQLITE_CREATE_TABLE, "sync_state", ""},
                                          Denial{SQLITE_PRAGMA, "user_version", "1"},
                                          Denial{SQLITE_TRANSACTION, "COMMIT", ""}}) {
        TempDirectory directory;
        const auto path = directory.path() / "rollback.sqlite3";
        Database db(path);
        expect(configure_connection(db.get()).has_value(), "test connection should configure");
        const auto before = db.catalog();
        auto denial = failure;
        sqlite3_set_authorizer(db.get(), &deny_operation, &denial);
        auto result = ensure_schema(db.get(), OpenMode::read_write_create);
        sqlite3_set_authorizer(db.get(), nullptr, nullptr);
        expect(denial.triggered, "failure injection should actually intercept the operation");
        expect(!result && result.error().code == ErrorCode::storage_error,
               "injected failure should be reported");
        expect(
            result.error().stage == (failure.action == SQLITE_TRANSACTION ? "storage.transaction"
                                                                          : "storage.initialize"),
            "failure stage should identify the operation");
        expect(sqlite3_get_autocommit(db.get()) == 1,
               "failed initialization should leave no active transaction");
        expect(db.catalog() == before && db.integer("PRAGMA user_version") == 0,
               "failed initialization must roll back every new object and version");
        Database persisted(path, SQLITE_OPEN_READONLY);
        expect(persisted.catalog() == before && persisted.integer("PRAGMA user_version") == 0,
               "rollback should survive independent reopening");
        expect(ensure_schema(db.get(), OpenMode::read_write_create).has_value(),
               "initialization should succeed after the fault is removed");
    }
}

void test_connection_configuration_and_wal_failure() {
    using namespace mailai::store_detail;
    TempDirectory directory;
    const auto path = directory.path() / "wal.sqlite3";
    Database db(path);
    expect(configure_connection(db.get()).has_value(), "connection should configure");
    expect(db.integer("PRAGMA foreign_keys") == 1,
           "connection should actually enforce foreign keys");
    expect(db.integer("PRAGMA busy_timeout") == 1000, "SQLite busy timeout should be 1000ms");
    expect(ensure_schema(db.get(), OpenMode::read_write_create).has_value(),
           "schema should commit before WAL");
    insert_account(db);
    Denial denial{SQLITE_PRAGMA, "journal_mode", "WAL"};
    sqlite3_set_authorizer(db.get(), &deny_operation, &denial);
    auto failed = configure_wal(db.get());
    sqlite3_set_authorizer(db.get(), nullptr, nullptr);
    expect(denial.triggered && !failed && failed.error().stage == "storage.configure",
           "WAL failure should be a configuration error");
    expect(
        db.integer("PRAGMA user_version") == 1 && db.integer("SELECT count(*) FROM accounts") == 1,
        "WAL failure must retain committed schema and account data");
    expect(SqliteStore::open(path, OpenMode::read_only).has_value(),
           "valid non-WAL version 1 should remain readable");
    expect(configure_wal(db.get()).has_value() && db.text("PRAGMA journal_mode") == "wal",
           "WAL should be enabled after the fault is removed");
    Denial foreign_keys{SQLITE_PRAGMA, "foreign_keys", "ON"};
    sqlite3_set_authorizer(db.get(), &deny_operation, &foreign_keys);
    auto configure_failed = configure_connection(db.get());
    sqlite3_set_authorizer(db.get(), nullptr, nullptr);
    expect(foreign_keys.triggered && !configure_failed &&
               configure_failed.error().stage == "storage.configure",
           "foreign key configuration failure must not be ignored");
}

void test_busy_writer_and_reader() {
    TempDirectory directory;
    const auto path = directory.path() / "busy.sqlite3";
    initialize(path);
    Database lock(path);
    lock.execute("BEGIN IMMEDIATE");
    expect(SqliteStore::open(path, OpenMode::read_only).has_value(),
           "WAL readers should work while another writer holds the lock");
    const auto start = std::chrono::steady_clock::now();
    const auto busy = SqliteStore::open(path, OpenMode::read_write_create);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(!busy && busy.error().code == ErrorCode::busy &&
               busy.error().stage == "storage.transaction",
           "write contention should report busy at BEGIN");
    expect(elapsed >= std::chrono::milliseconds{700} && elapsed < std::chrono::seconds{5},
           "busy wait should be finite and near the configured timeout");
    lock.execute("ROLLBACK");
    expect(SqliteStore::open(path, OpenMode::read_write_create).has_value(),
           "opening should recover after the external lock is released");
}

void test_constraints_and_raw_bytes() {
    using namespace mailai::store_detail;
    TempDirectory directory;
    const auto path = directory.path() / "constraints.sqlite3";
    initialize(path);
    Database db(path);
    expect(configure_connection(db.get()).has_value(), "constraint connection should configure");
    const auto rejected = [&](const char* sql) {
        const int rc = sqlite3_exec(db.get(), sql, nullptr, nullptr, nullptr);
        expect((rc & 0xff) == SQLITE_CONSTRAINT,
               "invalid write should be rejected by a real SQLite constraint");
    };
    rejected("INSERT INTO sync_state(account_id,folder) VALUES('missing','INBOX')");
    insert_account(db);
    rejected(
        "INSERT INTO sync_state(account_id,folder,last_discovered_uid) VALUES('school','INBOX',1)");
    db.execute(
        "INSERT INTO sync_state(account_id,folder,uid_validity) VALUES('school','INBOX',100)");
    rejected(
        "INSERT INTO sync_pending(account_id,folder,uid_validity,uid) "
        "VALUES('school','INBOX',100,0)");
    rejected(
        "INSERT INTO sync_pending(account_id,folder,uid_validity,uid) "
        "VALUES('school','other',100,1)");
    db.execute(
        "INSERT INTO sync_pending(account_id,folder,uid_validity,uid) "
        "VALUES('school','INBOX',100,1)");
    rejected(
        "INSERT INTO sync_pending(account_id,folder,uid_validity,uid) "
        "VALUES('school','INBOX',100,1)");
    constexpr const char* insert_message =
        "INSERT INTO "
        "messages(account_id,folder,uid_validity,uid,sender,subject,body,received_at_utc,raw,parse_"
        "state) VALUES('school','INBOX',100,1,'sender','subject','body',50,X'610062','parsed')";
    db.execute(insert_message);
    expect(db.text("SELECT typeof(raw)||':'||hex(raw) FROM messages") == "blob:610062",
           "raw should preserve NUL as a BLOB");
    rejected(insert_message);
    rejected("UPDATE messages SET parse_state='failed'");
    db.execute(
        "UPDATE messages SET parse_state='failed',subject='(解析失败)',body='',parse_error='test "
        "failure'");
    rejected("UPDATE messages SET parse_state='parsed'");
    rejected("INSERT INTO attachments VALUES(999,0,'file','text/plain',3)");
    rejected("INSERT INTO attachments VALUES(1,0,'file','text/plain',-1)");
    db.execute("INSERT INTO attachments VALUES(1,0,'file','text/plain',3)");
    rejected("INSERT INTO attachments VALUES(1,0,'duplicate','text/plain',3)");
    rejected("DELETE FROM accounts WHERE id='school'");
    db.execute("DELETE FROM messages WHERE id=1");
    expect(db.integer("SELECT count(*) FROM attachments") == 0,
           "deleting a message should cascade to attachments");
    db.execute(insert_message);
    expect(db.integer("SELECT id FROM messages") > 1,
           "AUTOINCREMENT must not reuse a deleted local ID");
    expect(SqliteStore::open(path, OpenMode::read_only).has_value(),
           "ordinary table data must not affect structure validation");
}

void test_memory_and_statistics() {
    using namespace mailai::store_detail;
    auto memory = SqliteStore::open(":memory:", OpenMode::read_write_create);
    expect(memory.has_value(), "in-memory schema should initialize without requiring WAL");
    expect(memory->ensure_account({"school", "school", "test@example.invalid"}, settings())
               .has_value(),
           "in-memory store should support writes");
    TempDirectory directory;
    const auto path = directory.path() / "statistics.sqlite3";
    initialize(path);
    Database db(path);
    insert_account(db);
    db.execute("ANALYZE; VACUUM;");
    expect(db.integer("SELECT count(*) FROM sqlite_schema WHERE name='sqlite_stat1'") == 1,
           "statistics fixture should actually contain a statistics table");
    for (auto mode : modes)
        expect(SqliteStore::open(path, mode).has_value(),
               "VACUUM and ANALYZE should not invalidate the application format");
}

void test_corrupt_schema() {
    TempDirectory directory;
    const auto path = directory.path() / "corrupt-schema.sqlite3";
    initialize(path);
    {
        Database db(path);
        db.execute(
            "PRAGMA writable_schema=ON; UPDATE sqlite_schema SET sql='malformed sql' WHERE "
            "name='accounts'; PRAGMA writable_schema=OFF;");
    }
    for (auto mode : modes) expect_storage_error(SqliteStore::open(path, mode));
    expect(std::filesystem::file_size(path) > 0,
           "corrupt schema file must not be removed or replaced with an empty file");
}
}  // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[]{
        {"initialize_and_reopen", test_initialize_and_reopen},
        {"original_version_one_reopens", test_original_version_one_reopens},
        {"empty_and_missing_paths", test_empty_and_missing_paths},
        {"unversioned_account_database_is_preserved",
         test_unversioned_account_database_is_preserved},
        {"unsupported_versions", test_unsupported_versions},
        {"unversioned_schema_variants", test_unversioned_schema_variants},
        {"unversioned_complete_database_is_preserved",
         test_unversioned_complete_database_is_preserved},
        {"version_one_without_schema_is_rejected", test_version_one_without_schema_is_rejected},
        {"version_one_definition_variants", test_version_one_definition_variants},
        {"version_one_catalog_variants", test_version_one_catalog_variants},
        {"non_database_file", test_non_database_file},
        {"account_save_and_identity_protection", test_account_save_and_identity_protection},
        {"schema_failures_roll_back", test_schema_failures_roll_back},
        {"connection_configuration_and_wal_failure", test_connection_configuration_and_wal_failure},
        {"busy_writer_and_reader", test_busy_writer_and_reader},
        {"constraints_and_raw_bytes", test_constraints_and_raw_bytes},
        {"memory_and_statistics", test_memory_and_statistics},
        {"corrupt_schema", test_corrupt_schema}};
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

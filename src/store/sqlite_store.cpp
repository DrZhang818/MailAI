#include "mailai/store.h"

#include <memory>
#include <string_view>

#include <sqlite3.h>
#include <utility>

namespace mailai {

namespace {

Status execute_sql(sqlite3* db, const char* sql) {
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);

    if (rc != SQLITE_OK) {
        return std::unexpected(
            Error{
                ErrorCode::storage_error,
                sqlite3_errmsg(db),
                std::nullopt,
                std::nullopt,
                "storage.sql"
            }
        );
    }

    return {};
}

} // namespace

SqliteStore::SqliteStore(sqlite3* db) noexcept : db_(db) {}

SqliteStore::~SqliteStore() {
    sqlite3_close_v2(db_);
}

SqliteStore::SqliteStore(SqliteStore&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

SqliteStore& SqliteStore::operator=(SqliteStore&& other) noexcept {
    if (this != &other) {
        sqlite3_close_v2(db_);
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

Result<SqliteStore> SqliteStore::open(const std::filesystem::path& path, OpenMode mode) {
    int flags = SQLITE_OPEN_READONLY;

    if (mode == OpenMode::read_write_create) {
        flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    }

    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);

    SqliteStore store(db);

    if (rc != SQLITE_OK) {
        return std::unexpected(
            Error{
                ErrorCode::storage_error,
                db ? sqlite3_errmsg(db) : "Failed to allocate SQLite connection",
                std::nullopt,
                std::nullopt,
                "storage.open"
            }
        );
    }

    if (mode == OpenMode::read_write_create) {
        constexpr const char* sql = R"sql(
            CREATE TABLE IF NOT EXISTS accounts (
                id TEXT PRIMARY KEY NOT NULL,
                display_name TEXT NOT NULL,
                address TEXT NOT NULL,
                imap_host TEXT NOT NULL,
                imap_port INTEGER NOT NULL,
                imap_username TEXT NOT NULL,
                credential_env TEXT NOT NULL,
                tls_mode TEXT NOT NULL
            );
        )sql";

        const auto status = execute_sql(db, sql);

        if (!status) {
            return std::unexpected(status.error());
        }
    }

    return store;
};

Status SqliteStore::ensure_account(const Account& account, const ImapSettings& imap) {
    const auto fail = [&](ErrorCode code, const char* message) -> Status {
        return std::unexpected(Error{code, message, account.id, std::nullopt, "storage.account"});
    };

    if (account.id.empty() || account.address.empty() || imap.host.empty() ||
        imap.username.empty() || imap.credential_env.empty() || imap.port == 0) {
        return fail(ErrorCode::invalid_argument, "Required account settings are missing");
    }

    if (imap.tls_mode != TlsMode::implicit && imap.tls_mode != TlsMode::starttls) {
        return fail(ErrorCode::invalid_argument, "Invalid TLS mode");
    }

    constexpr const char* sql = R"sql(
        INSERT INTO accounts (
            id, display_name, address, imap_host, imap_port,
            imap_username, credential_env, tls_mode
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(id) DO UPDATE SET
            display_name = excluded.display_name,
            address = excluded.address,
            credential_env = excluded.credential_env,
            tls_mode = excluded.tls_mode
        WHERE accounts.imap_host = excluded.imap_host
          AND accounts.imap_port = excluded.imap_port
          AND accounts.imap_username = excluded.imap_username;
    )sql";

    sqlite3_stmt* raw_statement = nullptr;
    const int prepare_rc = sqlite3_prepare_v2(db_, sql, -1, &raw_statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{
        raw_statement,
        &sqlite3_finalize
    };
    if (prepare_rc != SQLITE_OK) {
        return fail(ErrorCode::storage_error, sqlite3_errmsg(db_));
    }

    const std::string_view tls = imap.tls_mode == TlsMode::implicit ? "implicit" : "starttls";
    const std::pair<int, std::string_view> fields[]{
        {1, account.id},
        {2, account.display_name},
        {3, account.address},
        {4, imap.host},
        {6, imap.username},
        {7, imap.credential_env},
        {8, tls}
    };

    for (const auto& [index, value] : fields) {
        const int rc = sqlite3_bind_text64(
            statement.get(),
            index,
            value.data(),
            value.size(),
            SQLITE_TRANSIENT,
            SQLITE_UTF8
        );
        if (rc != SQLITE_OK) {
            return fail(ErrorCode::storage_error, sqlite3_errmsg(db_));
        }
    }

    if (sqlite3_bind_int(statement.get(), 5, imap.port) != SQLITE_OK) {
        return fail(ErrorCode::storage_error, sqlite3_errmsg(db_));
    }

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return fail(ErrorCode::storage_error, sqlite3_errmsg(db_));
    }

    if (sqlite3_changes(db_) == 0) {
        return fail(ErrorCode::config_error, "Account IMAP identity does not match saved settings");
    }

    return {};
}

} // namespace mailai
#include <string_view>
#include <utility>

#include "mailai/store.h"
#include "schema.h"
#include "sqlite_utils.h"

namespace mailai {

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
    const int flags = mode == OpenMode::read_write_create
                          ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                          : SQLITE_OPEN_READONLY;
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
    SqliteStore store(db);
    if (rc != SQLITE_OK)
        return std::unexpected(store_detail::sqlite_error(db, rc, "storage.open"));

    auto configured = store_detail::configure_connection(db);
    if (!configured)
        return std::unexpected(configured.error());
    auto schema = store_detail::ensure_schema(db, mode);
    if (!schema)
        return std::unexpected(schema.error());
    if (mode == OpenMode::read_write_create) {
        auto wal = store_detail::configure_wal(db);
        if (!wal)
            return std::unexpected(wal.error());
    }
    return store;
}

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

    auto prepared = store_detail::prepare(db_, sql, "storage.account");
    if (!prepared) {
        auto error = prepared.error();
        error.account_id = account.id;
        return std::unexpected(std::move(error));
    }
    auto statement = std::move(*prepared);

    const std::string_view tls = imap.tls_mode == TlsMode::implicit ? "implicit" : "starttls";
    const std::pair<int, std::string_view> fields[]{
        {1, account.id}, {2, account.display_name}, {3, account.address},
        {4, imap.host},  {6, imap.username},        {7, imap.credential_env},
        {8, tls}};

    for (const auto& [index, value] : fields) {
        const int rc = sqlite3_bind_text64(statement.get(), index, value.data(), value.size(),
                                           SQLITE_TRANSIENT, SQLITE_UTF8);
        if (rc != SQLITE_OK) {
            auto error = store_detail::sqlite_error(db_, rc, "storage.account");
            error.account_id = account.id;
            return std::unexpected(std::move(error));
        }
    }

    const int bind_rc = sqlite3_bind_int(statement.get(), 5, imap.port);
    if (bind_rc != SQLITE_OK) {
        auto error = store_detail::sqlite_error(db_, bind_rc, "storage.account");
        error.account_id = account.id;
        return std::unexpected(std::move(error));
    }

    const int step_rc = sqlite3_step(statement.get());
    if (step_rc != SQLITE_DONE) {
        auto error = store_detail::sqlite_error(db_, step_rc, "storage.account");
        error.account_id = account.id;
        return std::unexpected(std::move(error));
    }

    if (sqlite3_changes(db_) == 0) {
        return fail(ErrorCode::config_error, "Account IMAP identity does not match saved settings");
    }

    return {};
}

}  // namespace mailai

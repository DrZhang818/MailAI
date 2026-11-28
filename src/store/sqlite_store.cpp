#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "mailai/store.h"
#include "schema.h"
#include "sqlite_rows.h"
#include "sqlite_utils.h"

#include "mailai/error_message.h"

namespace mailai {
using store_detail::decode_attachment;
using store_detail::decode_inbox_item;
using store_detail::decode_mail;
using store_detail::decode_scalar;
using store_detail::SqlContext;

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
    if (rc != SQLITE_OK) return std::unexpected(store_detail::sqlite_error(db, rc, "storage.open"));

    auto configured = store_detail::configure_connection(db);
    if (!configured) return std::unexpected(configured.error());

    auto schema = store_detail::ensure_schema(db, mode);
    if (!schema) return std::unexpected(schema.error());

    if (mode == OpenMode::read_write_create) {
        auto wal = store_detail::configure_wal(db);
        if (!wal) return std::unexpected(wal.error());
    }

    return store;
}

Status SqliteStore::ensure_account(const Account& account, const ImapSettings& imap) {
    const SqlContext context{db_, "storage.account", account.id};
    if (account.id.empty() || account.address.empty() || imap.host.empty() ||
        imap.username.empty() || imap.credential_env.empty() || imap.port == 0) {
        return context.fail(ErrorCode::invalid_argument, error_message::storage::required_account_settings);
    }
    if (imap.tls_mode != TlsMode::implicit && imap.tls_mode != TlsMode::starttls) {
        return context.fail(ErrorCode::invalid_argument, error_message::storage::invalid_tls_mode);
    }
    if (!valid_credential_reference(imap.credential_env)) {
        return context.fail(ErrorCode::config_error, error_message::config::invalid_credential_reference);
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

    const std::string_view tls = imap.tls_mode == TlsMode::implicit ? "implicit" : "starttls";
    auto saved = context.execute(
        sql,
        {
            account.id,
            account.display_name,
            account.address,
            imap.host,
            imap.port,
            imap.username,
            imap.credential_env,
            tls
        }
    );
    if (!saved) return saved;

    if (context.changes() == 0)
        return context.fail(
            ErrorCode::config_error, error_message::storage::account_identity_mismatch);

    return {};
}

Status SqliteStore::check_account_identity(const Account& account, const ImapSettings& imap) {
    const SqlContext context{db_, "storage.account", account.id};
    auto found = context.query_one(
        "SELECT imap_host = ?2 AND imap_port = ?3 AND imap_username = ?4 FROM accounts WHERE id = ?1",
        {account.id, imap.host, imap.port, imap.username}, decode_scalar<bool>);
    if (!found)
        return std::unexpected(found.error());
    if (*found && !**found)
        return context.fail(ErrorCode::config_error, error_message::storage::account_identity_mismatch);
    return {};
}

Result<std::vector<InboxItem>> SqliteStore::list(const InboxQuery& query) {
    const SqlContext context{db_, "storage.list", query.account_id};
    auto transaction = context.begin(store_detail::TransactionMode::read);
    if (!transaction) return std::unexpected(transaction.error());

    if (query.account_id) {
        auto account = context.query_one(
            "SELECT 1 FROM accounts WHERE id=?1", {*query.account_id}, decode_scalar<std::int64_t>
        );
        if (!account) return std::unexpected(account.error());
        if (!*account) return context.fail(ErrorCode::unknown_account, error_message::common::unknown_account);
    }

    constexpr const char* sql = R"sql(
        SELECT m.id, m.account_id, a.display_name, m.sender, m.subject,
               m.received_at_utc, m.parse_state, m.historical
        FROM messages AS m
        JOIN accounts AS a ON m.account_id = a.id
        WHERE (?1 IS NULL OR m.account_id = ?1)
          AND (?2 IS NULL OR instr(lower(m.sender),lower(?2)) > 0
                          OR instr(lower(m.subject),lower(?2)) > 0
                          OR instr(lower(m.body),lower(?2)) > 0)
        ORDER BY m.received_at_utc DESC, m.id ASC
    )sql";
    using store_detail::SqlValue;
    const SqlValue account = query.account_id ? SqlValue{std::string_view{*query.account_id}}
                                             : SqlValue{nullptr};
    const SqlValue keyword = query.keyword ? SqlValue{std::string_view{*query.keyword}}
                                           : SqlValue{nullptr};
    auto items = context.query_all(sql, {account, keyword}, decode_inbox_item);
    if (!items) return std::unexpected(items.error());

    auto committed = context.commit(*transaction);
    if (!committed) return std::unexpected(committed.error());

    return items;
}

Result<Mail> SqliteStore::find(MailId id) {
    const SqlContext context{db_, "storage.find", {}, id};
    if (id == 0) return context.fail(ErrorCode::invalid_argument, error_message::common::invalid_mail_id, id);
    if (id > static_cast<MailId>(std::numeric_limits<std::int64_t>::max())) return context.fail(ErrorCode::mail_not_found, error_message::common::mail_not_found, id);

    auto transaction = context.begin(store_detail::TransactionMode::read);
    if (!transaction) return std::unexpected(transaction.error());

    constexpr const char* sql = R"sql(
        SELECT account_id, sender, subject, body, received_at_utc,
               folder, uid_validity, uid, parse_state, parse_error, historical,
               message_id, in_reply_to, references_header
        FROM messages
        WHERE id = ?1
    )sql";

    auto found = context.query_one(sql, {static_cast<std::int64_t>(id)}, decode_mail);
    if (!found) return std::unexpected(found.error());
    if (!*found) return context.fail(ErrorCode::mail_not_found, error_message::common::mail_not_found, id);

    Mail mail = std::move(**found);
    mail.id = id;

    constexpr const char* attachment_sql = R"sql(
        SELECT name, media_type, size_bytes
        FROM attachments
        WHERE mail_id = ?1
        ORDER BY ordinal ASC
    )sql";

    auto attachments = context.query_all(attachment_sql, {static_cast<std::int64_t>(id)}, decode_attachment);
    if (!attachments) return std::unexpected(attachments.error());
    mail.attachments = std::move(*attachments);

    auto committed = context.commit(*transaction);
    if (!committed) return std::unexpected(committed.error());

    return mail;
}

Result<std::string> SqliteStore::read_raw(MailId id) {
    const SqlContext context{db_, "storage.raw", {}, id};
    if (id == 0)
        return context.fail(ErrorCode::invalid_argument, error_message::common::invalid_mail_id);
    if (id > static_cast<MailId>(std::numeric_limits<std::int64_t>::max()))
        return context.fail(ErrorCode::mail_not_found, error_message::common::mail_not_found);
    auto found = context.query_one("SELECT raw FROM messages WHERE id = ?1",
        {static_cast<std::int64_t>(id)}, decode_scalar<store_detail::Blob>);
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return context.fail(ErrorCode::mail_not_found, error_message::common::mail_not_found);
    return std::move((**found).bytes);
}

}  // namespace mailai

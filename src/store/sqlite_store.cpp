#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "mailai/store.h"
#include "schema.h"
#include "sqlite_rows.h"
#include "sqlite_utils.h"

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
        return context.fail(ErrorCode::invalid_argument, "Required account settings are missing");
    }
    if (imap.tls_mode != TlsMode::implicit && imap.tls_mode != TlsMode::starttls) {
        return context.fail(ErrorCode::invalid_argument, "Invalid TLS mode");
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
            ErrorCode::config_error, "Account IMAP identity does not match saved settings");

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
        if (!*account) return context.fail(ErrorCode::unknown_account, "账号不存在");
    }

    std::string sql = R"sql(
        SELECT m.id, m.account_id, a.display_name, m.sender, m.subject,
               m.received_at_utc, m.parse_state, m.historical
        FROM messages AS m
        JOIN accounts AS a ON m.account_id = a.id
    )sql";
    if (query.account_id)
        sql += " WHERE m.account_id = ?1";
    sql += " ORDER BY m.received_at_utc DESC, m.id ASC";

    auto items = query.account_id ? context.query_all(sql, {*query.account_id}, decode_inbox_item)
                                 : context.query_all(sql, {}, decode_inbox_item);
    if (!items) return std::unexpected(items.error());

    auto committed = context.commit(*transaction);
    if (!committed) return std::unexpected(committed.error());

    return items;
}

Result<Mail> SqliteStore::find(MailId id) {
    const SqlContext context{db_, "storage.find", {}, id};
    if (id == 0) return context.fail(ErrorCode::invalid_argument, "邮件 ID 必须大于 0", id);
    if (id > static_cast<MailId>(std::numeric_limits<std::int64_t>::max())) return context.fail(ErrorCode::mail_not_found, "邮件不存在", id);

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
    if (!*found) return context.fail(ErrorCode::mail_not_found, "邮件不存在", id);

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

}  // namespace mailai

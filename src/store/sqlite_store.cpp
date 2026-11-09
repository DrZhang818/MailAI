#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "mailai/store.h"
#include "schema.h"
#include "sqlite_utils.h"

namespace mailai {
using store_detail::column_text;
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

    if (sqlite3_changes(db_) == 0)
        return context.fail(
            ErrorCode::config_error, "Account IMAP identity does not match saved settings");

    return {};
}

Result<std::vector<InboxItem>> SqliteStore::list(const InboxQuery& query) {
    constexpr std::string_view stage = "storage.list";
    const SqlContext context{db_, stage};
    auto transaction = context.begin(store_detail::TransactionMode::read);
    if (!transaction) return std::unexpected(transaction.error());

    if (query.account_id) {
        auto account = context.prepare("SELECT 1 FROM accounts WHERE id=?1", {*query.account_id});
        if (!account) return std::unexpected(account.error());

        auto found = context.next(*account);
        if (!found) return std::unexpected(found.error());
        if (!*found)
            return SqlContext{db_, stage, query.account_id}.fail(
                ErrorCode::unknown_account, "账号不存在");
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

    auto statement = context.prepare(sql);
    if (!statement) return std::unexpected(statement.error());

    if (query.account_id) {
        auto bound = context.bind(*statement, 1, {*query.account_id});
        if (!bound) return std::unexpected(bound.error());
    }

    std::vector<InboxItem> items;
    while (true) {
        auto row = context.next(*statement);
        if (!row) return std::unexpected(row.error());
        if (!*row) break;

        const auto id    = sqlite3_column_int64(statement->get(), 0);
        const auto state = column_text(statement->get(), 6);
        if (id <= 0 || (state != "parsed" && state != "failed"))
            return SqlContext{db_, stage, query.account_id}.fail(
                ErrorCode::storage_error, "Invalid stored mail fields");

        items.emplace_back(
            static_cast<MailId>(id),
            column_text(statement->get(), 1),
            column_text(statement->get(), 2),
            column_text(statement->get(), 3),
            column_text(statement->get(), 4),
            sqlite3_column_int64(statement->get(), 5),
            state == "parsed" ? ParseState::parsed : ParseState::failed,
            sqlite3_column_int(statement->get(), 7) != 0
        );
    }

    auto committed = context.commit(*transaction);
    if (!committed) return std::unexpected(committed.error());

    return items;
}

Result<Mail> SqliteStore::find(MailId id) {
    const SqlContext context{db_, "storage.find"};
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

    auto statement = context.prepare(sql, {static_cast<std::int64_t>(id)});
    if (!statement) return std::unexpected(statement.error());

    auto found = context.next(*statement);
    if (!found) return std::unexpected(found.error());
    if (!*found) return context.fail(ErrorCode::mail_not_found, "邮件不存在", id);

    const auto validity = sqlite3_column_int64(statement->get(), 6);
    const auto uid      = sqlite3_column_int64(statement->get(), 7);
    const auto max_uid  = static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
    const auto state    = column_text(statement->get(), 8);
    if (validity < 1 || validity > max_uid || uid < 1 || uid > max_uid ||
        (state != "parsed" && state != "failed")) {
        return context.fail(ErrorCode::storage_error, "Invaild stored mail fields", id);
    }

    Mail mail;
    mail.id              = id;
    mail.account_id      = column_text(statement->get(), 0);
    mail.sender          = column_text(statement->get(), 1);
    mail.subject         = column_text(statement->get(), 2);
    mail.body            = column_text(statement->get(), 3);
    mail.received_at_utc = sqlite3_column_int64(statement->get(), 4);
    mail.folder          = column_text(statement->get(), 5);
    mail.uid_validity    = static_cast<std::uint32_t>(validity);
    mail.uid             = static_cast<std::uint32_t>(uid);
    mail.parse_state     = state == "parsed" ? ParseState::parsed : ParseState::failed;
    mail.parse_error     = column_text(statement->get(), 9);
    mail.historical      = sqlite3_column_int(statement->get(), 10) != 0;
    mail.message_id      = column_text(statement->get(), 11);
    mail.in_reply_to     = column_text(statement->get(), 12);
    mail.references      = column_text(statement->get(), 13);

    auto finished = context.finish(*statement);
    if (!finished) return std::unexpected(finished.error());

    constexpr const char* attachment_sql = R"sql(
        SELECT name, media_type, size_bytes
        FROM attachments
        WHERE mail_id = ?1
        ORDER BY ordinal ASC
    )sql";

    auto attachments = context.prepare(attachment_sql, {static_cast<std::int64_t>(id)});
    if (!attachments) return std::unexpected(attachments.error());

    while (true) {
        auto row = context.next(*attachments);
        if (!row) return std::unexpected(row.error());
        if (!*row) break;

        const auto size = sqlite3_column_int64(attachments->get(), 2);
        if (size < 0) return context.fail(ErrorCode::storage_error, "Invalid stored attachment size", id);

        mail.attachments.push_back(AttachmentInfo{
            column_text(attachments->get(), 0),
            column_text(attachments->get(), 1),
            static_cast<std::uint64_t>(size)
        });
    }

    auto committed = context.commit(*transaction);
    if (!committed) return std::unexpected(committed.error());

    return mail;
}

}  // namespace mailai

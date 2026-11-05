#include <limits>
#include <string>
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
            statement.get(), index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
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

Result<std::vector<InboxItem>> SqliteStore::list(const InboxQuery& query) {
    constexpr std::string_view stage = "storage.list";

    auto transaction = store_detail::Transaction::begin(db_, store_detail::TransactionMode::read);
    if (!transaction)
        return std::unexpected(transaction.error());

    // 指定帐号是时，先检查账号是否存在
    if (query.account_id) {
        auto account = store_detail::prepare(db_, "SELECT 1 FROM accounts WHERE id=?1", stage);
        if (!account)
            return std::unexpected(account.error());

        const auto& id = *query.account_id;
        int rc = sqlite3_bind_text64(
            account->get(), 1, id.data(), id.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        if (rc != SQLITE_OK)
            return std::unexpected(store_detail::sqlite_error(db_, rc, stage));

        rc = sqlite3_step(account->get());
        if (rc == SQLITE_DONE)
            return std::unexpected(Error{
                ErrorCode::unknown_account,
                "账号不存在",
                query.account_id,
                std::nullopt,
                std::string{stage}
            });
        if (rc != SQLITE_ROW)
            return std::unexpected(store_detail::sqlite_error(db_, rc, stage));
    }

    // 只拼接固定 SQL 片段，账号值通过参数绑定传入
    std::string sql = R"sql(
        SELECT m.id, m.account_id, a.display_name, m.sender, m.subject,
               m.received_at_utc, m.parse_state, m.historical
        FROM messages AS m
        JOIN accounts AS a ON m.account_id = a.id
    )sql";

    if (query.account_id)
        sql += " WHERE m.account_id = ?1";
    sql += " ORDER BY m.received_at_utc DESC, m.id ASC";

    auto prepared = store_detail::prepare(db_, sql, stage);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto statement = std::move(*prepared);

    if (query.account_id) {
        const auto& id = *query.account_id;
        const int rc = sqlite3_bind_text64(
            statement.get(), 1, id.data(), id.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        if (rc != SQLITE_OK)
            return std::unexpected(store_detail::sqlite_error(db_, rc, stage));
    }

    // 将 SQLite 管理的文本复制为程序自己拥有的字符串
    const auto text = [](sqlite3_stmt* row, int column) -> std::string {
        const auto* value = sqlite3_column_text(row, column);
        if (!value)
            return {};
        return {
            reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(sqlite3_column_bytes(row, column))
        };
    };

    std::vector<InboxItem> items;
    int rc;

    while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto id = sqlite3_column_int64(statement.get(), 0);
        const auto state = text(statement.get(), 6);

        if (id <= 0 || (state != "parsed" && state != "failed"))
            return std::unexpected(Error{
                ErrorCode::storage_error,
                "Invalid stored mail fields",
                query.account_id,
                std::nullopt,
                std::string{stage}
            });

        items.emplace_back(static_cast<MailId>(id),
            text(statement.get(), 1),
            text(statement.get(), 2),
            text(statement.get(), 3),
            text(statement.get(), 4),
            sqlite3_column_int64(statement.get(), 5),
            state == "parsed" ? ParseState::parsed : ParseState::failed,
            sqlite3_column_int(statement.get(), 7) != 0);
    }

    if (rc != SQLITE_DONE)
        return std::unexpected(store_detail::sqlite_error(db_, rc, stage));

    auto committed = transaction->commit();
    if (!committed)
        return std::unexpected(committed.error());

    return items;
}

Result<Mail> SqliteStore::find(MailId id) {
    constexpr std::string_view stage = "storage.find";
    const auto fail = [&](ErrorCode code, const char* message) -> Result<Mail> {
        return std::unexpected(Error{code, message, std::nullopt, id, std::string{stage}});
    };
    if (id == 0)
        return fail(ErrorCode::invalid_argument, "邮件 ID 必须大于 0");
    if (id > static_cast<MailId>(std::numeric_limits<std::int64_t>::max()))
        return fail(ErrorCode::mail_not_found, "邮件不存在");

    auto transaction = store_detail::Transaction::begin(db_, store_detail::TransactionMode::read);
    if (!transaction)
        return std::unexpected(transaction.error());

    const auto text = [](sqlite3_stmt* row, int column) -> std::string {
        const auto* value = sqlite3_column_text(row, column);
        if (!value)
            return {};
        return {
            reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(sqlite3_column_bytes(row, column))
        };
    };

    constexpr const char* sql = R"sql(
        SELECT account_id, sender, subject, body, received_at_utc,
               folder, uid_validity, uid, parse_state, parse_error, historical,
               message_id, in_reply_to, references_header
        FROM messages
        WHERE id = ?1
    )sql";

    auto prepared = store_detail::prepare(db_, sql, stage);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto statement = std::move(*prepared);

    int rc = sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(id));
    if (rc != SQLITE_OK)
        return std::unexpected(store_detail::sqlite_error(db_, rc, stage));

    rc = sqlite3_step(statement.get());
    if (rc == SQLITE_DONE)
        return fail(ErrorCode::mail_not_found, "邮件不存在");
    if (rc != SQLITE_ROW)
        return std::unexpected(store_detail::sqlite_error(db_, rc, stage));

    const auto validity = sqlite3_column_int64(statement.get(), 6);
    const auto uid = sqlite3_column_int64(statement.get(), 7);
    const auto max_uid = static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
    const auto state = text(statement.get(), 8);

    if (validity < 1 || validity > max_uid || uid < 1 || uid > max_uid ||
        (state != "parsed" && state != "failed")) {
        return fail(ErrorCode::storage_error, "Invaild stored mail fields");
    }

    Mail mail;
    mail.id = id;
    mail.account_id = text(statement.get(), 0);
    mail.sender = text(statement.get(), 1);
    mail.subject = text(statement.get(), 2);
    mail.body = text(statement.get(), 3);
    mail.received_at_utc = sqlite3_column_int64(statement.get(), 4);
    mail.folder = text(statement.get(), 5);
    mail.uid_validity = static_cast<std::uint32_t>(validity);
    mail.uid = static_cast<std::uint32_t>(uid);
    mail.parse_state = state == "parsed" ? ParseState::parsed : ParseState::failed;
    mail.parse_error = text(statement.get(), 9);
    mail.historical = sqlite3_column_int(statement.get(), 10) != 0;
    mail.message_id = text(statement.get(), 11);
    mail.in_reply_to = text(statement.get(), 12);
    mail.references = text(statement.get(), 13);

    rc = sqlite3_step(statement.get());
    if (rc != SQLITE_DONE)
        return std::unexpected(store_detail::sqlite_error(db_, rc, stage));

    constexpr const char* attachment_sql = R"sql(
        SELECT name, media_type, size_bytes
        FROM attachments
        WHERE mail_id = ?1
        ORDER BY ordinal ASC
    )sql";

    auto prepared_attachments = store_detail::prepare(db_, attachment_sql, stage);
    if (!prepared_attachments)
        return std::unexpected(prepared_attachments.error());
    auto attachments = std::move(*prepared_attachments);

    rc = sqlite3_bind_int64(attachments.get(), 1, static_cast<sqlite3_int64>(id));
    if (rc != SQLITE_OK)
        return std::unexpected(store_detail::sqlite_error(db_, rc, stage));

    while ((rc = sqlite3_step(attachments.get())) == SQLITE_ROW) {
        const auto size = sqlite3_column_int64(attachments.get(), 2);
        if (size < 0)
            return fail(ErrorCode::storage_error, "Invalid stored attachment size");

        mail.attachments.push_back(AttachmentInfo{
            text(attachments.get(), 0), text(attachments.get(), 1), static_cast<std::uint64_t>(size)
        });
    }

    if (rc != SQLITE_DONE)
        return std::unexpected(store_detail::sqlite_error(db_, rc, stage));

    auto committed = transaction->commit();
    if (!committed)
        return std::unexpected(committed.error());

    return mail;
}

}  // namespace mailai

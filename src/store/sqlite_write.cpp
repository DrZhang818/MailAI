#include <limits>
#include <string_view>
#include <utility>

#include "mailai/store.h"
#include "sqlite_utils.h"

namespace mailai {
namespace {
int bind_text(sqlite3_stmt* statement, int index, std::string_view value) {
    return sqlite3_bind_text64(statement,
        index,
        value.empty() ? "" : value.data(),
        value.size(),
        SQLITE_TRANSIENT,
        SQLITE_UTF8);
}

struct WriteContext {
    sqlite3* db;
    const AccountId& account;
    std::string_view stage;

    Error contextual(Error error) const {
        error.account_id = account;
        return error;
    }
    Error error(int rc) const {
        return contextual(store_detail::sqlite_error(db, rc, stage));
    }
    Error failure(ErrorCode code, const char* message) const {
        return {code, message, account, std::nullopt, std::string{stage}};
    }
    Result<store_detail::Statement> prepare(const char* sql) const {
        auto statement = store_detail::prepare(db, sql, stage);
        if (!statement)
            return std::unexpected(contextual(statement.error()));
        return statement;
    }
    Result<store_detail::Statement> scope(const char* sql, std::string_view folder) const {
        auto statement = prepare(sql);
        if (!statement)
            return std::unexpected(statement.error());
        const std::pair<int, std::string_view> fields[]{{1, account}, {2, folder}};
        for (const auto& [index, value] : fields) {
            const int rc = bind_text(statement->get(), index, value);
            if (rc != SQLITE_OK)
                return std::unexpected(error(rc));
        }
        return statement;
    }
    Result<store_detail::Statement> key(const char* sql, const RemoteKey& remote) const {
        auto statement = scope(sql, remote.folder);
        if (!statement)
            return std::unexpected(statement.error());
        const std::pair<int, std::uint32_t> fields[]{{3, remote.uid_validity}, {4, remote.uid}};
        for (const auto& [index, value] : fields) {
            const int rc = sqlite3_bind_int64(statement->get(), index, value);
            if (rc != SQLITE_OK)
                return std::unexpected(error(rc));
        }
        return statement;
    }
    Status done(sqlite3_stmt* statement) const {
        const int rc = sqlite3_step(statement);
        if (rc != SQLITE_DONE)
            return std::unexpected(error(rc));
        return {};
    }
};

bool valid_key(const RemoteKey& key) {
    return !key.account_id.empty() && !key.folder.empty() && key.uid_validity != 0 && key.uid != 0;
}

Status require_current_key(SqliteStore& store, const RemoteKey& key, const WriteContext& context) {
    auto state = store.state(key.account_id, key.folder);
    if (!state) {
        auto error = context.contextual(state.error());
        error.stage = std::string{context.stage};
        return std::unexpected(std::move(error));
    }
    if (!state->uid_validity || *state->uid_validity != key.uid_validity)
        return std::unexpected(context.failure(
            ErrorCode::invalid_argument, "Remote key does not match the current mailbox state"));
    return {};
}

std::string_view error_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::unknown_account:
            return "unknown_account";
        case ErrorCode::mail_not_found:
            return "mail_not_found";
        case ErrorCode::invalid_argument:
            return "invalid_argument";
        case ErrorCode::config_error:
            return "config_error";
        case ErrorCode::auth_error:
            return "auth_error";
        case ErrorCode::network_error:
            return "network_error";
        case ErrorCode::protocol_error:
            return "protocol_error";
        case ErrorCode::remote_gone:
            return "remote_gone";
        case ErrorCode::parse_error:
            return "parse_error";
        case ErrorCode::storage_error:
            return "storage_error";
        case ErrorCode::busy:
            return "busy";
    }
    return {};
}
}  // namespace

Result<MailId> SqliteStore::complete(const FetchedMail& mail, const Result<ParsedMail>& parsed) {
    const auto& remote = mail.key;
    const WriteContext context{db_, remote.account_id, "storage.complete"};
    if (!valid_key(remote))
        return std::unexpected(
            context.failure(ErrorCode::invalid_argument, "Invalid remote mail key"));
    if (!parsed &&
        (parsed.error().code != ErrorCode::parse_error || parsed.error().message.empty()))
        return std::unexpected(
            context.failure(ErrorCode::invalid_argument, "Invalid parsing failure"));
    constexpr auto max_integer = std::numeric_limits<std::int64_t>::max();
    if (parsed) {
        if (parsed->attachments.size() > static_cast<std::uint64_t>(max_integer))
            return std::unexpected(
                context.failure(ErrorCode::invalid_argument, "Too many attachments"));
        for (const auto& attachment : parsed->attachments) {
            if (attachment.size_bytes > static_cast<std::uint64_t>(max_integer))
                return std::unexpected(context.failure(ErrorCode::invalid_argument,
                    "Attachment size exceeds the SQLite integer range"));
        }
    }

    auto transaction = store_detail::Transaction::begin(db_, store_detail::TransactionMode::write);
    if (!transaction)
        return std::unexpected(context.contextual(transaction.error()));
    auto current = require_current_key(*this, remote, context);
    if (!current)
        return std::unexpected(current.error());

    constexpr const char* save_sql = R"sql(
        INSERT INTO messages (
            account_id, folder, uid_validity, uid, sender, subject, body,
            received_at_utc, raw, parse_state, parse_error, historical,
            message_id, in_reply_to, references_header
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, 0, ?12, ?13, ?14)
        ON CONFLICT(account_id, folder, uid_validity, uid) DO UPDATE SET
            sender = excluded.sender, subject = excluded.subject, body = excluded.body,
            received_at_utc = excluded.received_at_utc, raw = excluded.raw,
            parse_state = excluded.parse_state, parse_error = excluded.parse_error,
            historical = 0, message_id = excluded.message_id,
            in_reply_to = excluded.in_reply_to, references_header = excluded.references_header
    )sql";
    auto saved = context.key(save_sql, remote);
    if (!saved)
        return std::unexpected(saved.error());
    const ParsedMail* value = parsed ? &*parsed : nullptr;
    const std::string_view subject =
        value ? (value->subject.empty() ? std::string_view{"(无主题)"}
                                        : std::string_view{value->subject})
              : std::string_view{"(解析失败)"};
    const std::pair<int, std::string_view> text_fields[]{
        {5, value ? std::string_view{value->sender} : std::string_view{}},
        {6, subject},
        {7, value ? std::string_view{value->body} : std::string_view{}},
        {10, value ? "parsed" : "failed"},
        {11, value ? std::string_view{} : std::string_view{parsed.error().message}},
        {12, value ? std::string_view{value->message_id} : std::string_view{}},
        {13, value ? std::string_view{value->in_reply_to} : std::string_view{}},
        {14, value ? std::string_view{value->references} : std::string_view{}}
    };
    for (const auto& [index, text] : text_fields) {
        const int rc = bind_text(saved->get(), index, text);
        if (rc != SQLITE_OK)
            return std::unexpected(context.error(rc));
    }
    int rc = sqlite3_bind_int64(saved->get(), 8, mail.received_at_utc);
    if (rc != SQLITE_OK)
        return std::unexpected(context.error(rc));
    rc = sqlite3_bind_blob64(saved->get(), 9, mail.raw.data(), mail.raw.size(), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
        return std::unexpected(context.error(rc));
    auto status = context.done(saved->get());
    if (!status)
        return std::unexpected(status.error());

    auto lookup = context.key(
        "SELECT id FROM messages WHERE account_id=?1 AND folder=?2 "
        "AND uid_validity=?3 AND uid=?4",
        remote);
    if (!lookup)
        return std::unexpected(lookup.error());
    rc = sqlite3_step(lookup->get());
    if (rc != SQLITE_ROW)
        return std::unexpected(rc == SQLITE_DONE ? context.failure(ErrorCode::storage_error,
                                                       "Saved mail was not found")
                                                 : context.error(rc));
    const auto local_id = sqlite3_column_int64(lookup->get(), 0);
    if (sqlite3_column_type(lookup->get(), 0) != SQLITE_INTEGER || local_id <= 0)
        return std::unexpected(context.failure(ErrorCode::storage_error, "Invalid stored mail ID"));
    rc = sqlite3_step(lookup->get());
    if (rc != SQLITE_DONE)
        return std::unexpected(context.error(rc));

    auto removed = context.prepare("DELETE FROM attachments WHERE mail_id=?1");
    if (!removed)
        return std::unexpected(removed.error());
    rc = sqlite3_bind_int64(removed->get(), 1, local_id);
    if (rc != SQLITE_OK)
        return std::unexpected(context.error(rc));
    status = context.done(removed->get());
    if (!status)
        return std::unexpected(status.error());
    if (value && !value->attachments.empty()) {
        auto attachment = context.prepare(
            "INSERT INTO attachments "
            "(mail_id,ordinal,name,media_type,size_bytes) VALUES (?1,?2,?3,?4,?5)");
        if (!attachment)
            return std::unexpected(attachment.error());
        rc = sqlite3_bind_int64(attachment->get(), 1, local_id);
        if (rc != SQLITE_OK)
            return std::unexpected(context.error(rc));
        for (std::size_t ordinal = 0; ordinal < value->attachments.size(); ++ordinal) {
            const auto& item = value->attachments[ordinal];
            rc = sqlite3_bind_int64(attachment->get(), 2, static_cast<sqlite3_int64>(ordinal));
            if (rc != SQLITE_OK)
                return std::unexpected(context.error(rc));
            rc = bind_text(attachment->get(), 3, item.name);
            if (rc != SQLITE_OK)
                return std::unexpected(context.error(rc));
            rc = bind_text(attachment->get(), 4, item.media_type);
            if (rc != SQLITE_OK)
                return std::unexpected(context.error(rc));
            rc = sqlite3_bind_int64(
                attachment->get(), 5, static_cast<sqlite3_int64>(item.size_bytes));
            if (rc != SQLITE_OK)
                return std::unexpected(context.error(rc));
            status = context.done(attachment->get());
            if (!status)
                return std::unexpected(status.error());
            rc = sqlite3_reset(attachment->get());
            if (rc != SQLITE_OK)
                return std::unexpected(context.error(rc));
        }
    }

    constexpr const char* parse_failure_sql = R"sql(
        INSERT INTO sync_pending (
            account_id, folder, uid_validity, uid, retryable,
            last_error_code, last_error_stage, last_error_message
        ) VALUES (?1, ?2, ?3, ?4, 1, 'parse_error', ?5, ?6)
        ON CONFLICT(account_id, folder, uid_validity, uid) DO UPDATE SET
            last_error_code = excluded.last_error_code,
            last_error_stage = excluded.last_error_stage,
            last_error_message = excluded.last_error_message
    )sql";
    auto job = context.key(value ? "DELETE FROM sync_pending WHERE account_id=?1 AND folder=?2 AND "
                                   "uid_validity=?3 AND uid=?4"
                                 : parse_failure_sql,
        remote);
    if (!job)
        return std::unexpected(job.error());
    if (!value) {
        const std::pair<int, std::string_view> fields[]{
            {5, parsed.error().stage}, {6, parsed.error().message}
        };
        for (const auto& [index, text] : fields) {
            rc = bind_text(job->get(), index, text);
            if (rc != SQLITE_OK)
                return std::unexpected(context.error(rc));
        }
    }
    status = context.done(job->get());
    if (!status)
        return std::unexpected(status.error());
    status = transaction->commit();
    if (!status)
        return std::unexpected(context.contextual(status.error()));
    return static_cast<MailId>(local_id);
}

Status SqliteStore::fail(const RemoteKey& key, const Error& error) {
    const WriteContext context{db_, key.account_id, "storage.fail"};
    const auto name = error_name(error.code);
    if (!valid_key(key) || name.empty() || error.message.empty())
        return std::unexpected(
            context.failure(ErrorCode::invalid_argument, "Invalid failure report"));
    auto transaction = store_detail::Transaction::begin(db_, store_detail::TransactionMode::write);
    if (!transaction)
        return std::unexpected(context.contextual(transaction.error()));
    auto current = require_current_key(*this, key, context);
    if (!current)
        return current;
    constexpr const char* sql = R"sql(
        UPDATE sync_pending SET
            retryable = CASE WHEN ?5='remote_gone' THEN 0 ELSE retryable END,
            last_error_code = ?5, last_error_stage = ?6, last_error_message = ?7
        WHERE account_id=?1 AND folder=?2 AND uid_validity=?3 AND uid=?4
    )sql";
    auto statement = context.key(sql, key);
    if (!statement)
        return std::unexpected(statement.error());
    const std::pair<int, std::string_view> fields[]{
        {5, name}, {6, error.stage}, {7, error.message}
    };
    for (const auto& [index, text] : fields) {
        const int rc = bind_text(statement->get(), index, text);
        if (rc != SQLITE_OK)
            return std::unexpected(context.error(rc));
    }
    auto status = context.done(statement->get());
    if (!status)
        return status;
    if (sqlite3_changes(db_) == 0)
        return std::unexpected(
            context.failure(ErrorCode::invalid_argument, "Pending job was not found"));
    status = transaction->commit();
    if (!status)
        return std::unexpected(context.contextual(status.error()));
    return {};
}

Status SqliteStore::record_outcome(const AccountId& account_id,
    std::string_view folder,
    const Result<SyncSummary>& outcome,
    std::int64_t attempted_at) {
    const WriteContext context{db_, account_id, "storage.outcome"};
    if (account_id.empty() || folder.empty())
        return std::unexpected(
            context.failure(ErrorCode::invalid_argument, "Account and folder must not be empty"));
    auto transaction = store_detail::Transaction::begin(db_, store_detail::TransactionMode::write);
    if (!transaction)
        return std::unexpected(context.contextual(transaction.error()));
    auto current = state(account_id, folder);
    if (!current) {
        auto error = context.contextual(current.error());
        error.stage = std::string{context.stage};
        return std::unexpected(std::move(error));
    }
    constexpr const char* sql = R"sql(
        INSERT INTO sync_state(account_id, folder, last_attempt_at, last_success_at)
        VALUES (?1, ?2, ?3, CASE WHEN ?4 THEN ?3 ELSE NULL END)
        ON CONFLICT(account_id, folder) DO UPDATE SET
            last_attempt_at = excluded.last_attempt_at,
            last_success_at = CASE WHEN ?4 THEN excluded.last_success_at ELSE sync_state.last_success_at END
    )sql";
    auto statement = context.scope(sql, folder);
    if (!statement)
        return std::unexpected(statement.error());
    int rc = sqlite3_bind_int64(statement->get(), 3, attempted_at);
    if (rc != SQLITE_OK)
        return std::unexpected(context.error(rc));
    rc = sqlite3_bind_int(statement->get(), 4, outcome && outcome->failed == 0);
    if (rc != SQLITE_OK)
        return std::unexpected(context.error(rc));
    auto status = context.done(statement->get());
    if (!status)
        return status;
    status = transaction->commit();
    if (!status)
        return std::unexpected(context.contextual(status.error()));
    return {};
}

}  // namespace mailai

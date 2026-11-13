#include <array>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

#include "mailai/store.h"
#include "sqlite_sync.h"
#include "sqlite_utils.h"

namespace mailai {
using store_detail::BlobView;
using store_detail::SqlContext;
using store_detail::SqlValue;

namespace {
bool valid_key(const RemoteKey& key) {
    return !key.account_id.empty() && !key.folder.empty() && key.uid_validity != 0 && key.uid != 0;
}

Status require_current_key(const RemoteKey& key, const SqlContext& context) {
    auto state = store_detail::load_sync_state(context, key.account_id, key.folder);
    if (!state) return std::unexpected(state.error());
    if (!state->uid_validity || *state->uid_validity != key.uid_validity)
        return context.fail(
            ErrorCode::invalid_argument, "Remote key does not match the current mailbox state");
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

Status validate_completion(const SqlContext& context, const RemoteKey& remote, const Result<ParsedMail>& parsed) {
    if (!valid_key(remote)) return context.fail(ErrorCode::invalid_argument, "Invalid remote mail key");
    if (!parsed &&
        (parsed.error().code != ErrorCode::parse_error || parsed.error().message.empty())) {
        return context.fail(ErrorCode::invalid_argument, "Invalid parsing failure");
    }

    constexpr auto max_integer = std::numeric_limits<std::int64_t>::max();
    if (parsed) {
        if (parsed->attachments.size() > static_cast<std::uint64_t>(max_integer))
            return context.fail(ErrorCode::invalid_argument, "Too many attachments");
        for (const auto& attachment : parsed->attachments) {
            if (attachment.size_bytes > static_cast<std::uint64_t>(max_integer))
                return context.fail(ErrorCode::invalid_argument,
                    "Attachment size exceeds the SQLite integer range");
        }
    }

    return {};
}

Result<MailId> save_message(const SqlContext& context, const FetchedMail& mail, const Result<ParsedMail>& parsed) {
    const auto& remote = mail.key;
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
    const ParsedMail* value = parsed ? &*parsed : nullptr;
    const std::string_view subject =
        value ? (value->subject.empty() ? std::string_view{"(无主题)"}
                                        : std::string_view{value->subject})
              : std::string_view{"(解析失败)"};
    auto saved = context.execute(
        save_sql,
        {
            remote.account_id,
            remote.folder,
            remote.uid_validity,
            remote.uid,
            value ? std::string_view{value->sender} : std::string_view{},
            subject,
            value ? std::string_view{value->body} : std::string_view{},
            mail.received_at_utc,
            BlobView{mail.raw},
            value ? "parsed" : "failed",
            value ? std::string_view{} : std::string_view{parsed.error().message},
            value ? std::string_view{value->message_id} : std::string_view{},
            value ? std::string_view{value->in_reply_to} : std::string_view{},
            value ? std::string_view{value->references} : std::string_view{}
        }
    );
    if (!saved) return std::unexpected(saved.error());

    auto found = context.query_one(
        "SELECT id FROM messages WHERE account_id=?1 AND folder=?2 "
        "AND uid_validity=?3 AND uid=?4",
        {remote.account_id, remote.folder, remote.uid_validity, remote.uid},
        store_detail::decode_scalar<MailId>
    );
    if (!found) return std::unexpected(found.error());
    if (!*found) return context.fail(ErrorCode::storage_error, "Saved mail was not found");
    if (**found == 0) return context.fail(ErrorCode::storage_error, "Invalid stored mail ID");

    return **found;
}

Status replace_attachments(const SqlContext& context, std::int64_t local_id, const ParsedMail* value) {
    auto removed = context.execute("DELETE FROM attachments WHERE mail_id=?1", {local_id});
    if (!removed) return std::unexpected(removed.error());

    if (!value || value->attachments.empty()) return {};

    auto ordinals = std::views::iota(std::size_t{0}, value->attachments.size());
    auto rows = ordinals | std::views::transform([&](std::size_t ordinal) {
        const auto& item = value->attachments[ordinal];
        return std::array<SqlValue, 4>{
            static_cast<std::int64_t>(ordinal),
            item.name,
            item.media_type,
            static_cast<std::int64_t>(item.size_bytes)
        };
    });
    return context.execute_many(
        "INSERT INTO attachments "
        "(mail_id,ordinal,name,media_type,size_bytes) VALUES (?1,?2,?3,?4,?5)",
        {local_id}, rows
    );
}

Status update_pending_after_parse(const SqlContext& context, const RemoteKey& remote, const Result<ParsedMail>& parsed) {
    if (parsed) {
        return context.execute(
            "DELETE FROM sync_pending WHERE account_id=?1 AND folder=?2 AND "
            "uid_validity=?3 AND uid=?4",
            {remote.account_id, remote.folder, remote.uid_validity, remote.uid}
        );
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
    return context.execute(
        parse_failure_sql,
        {remote.account_id, remote.folder, remote.uid_validity, remote.uid, parsed.error().stage, parsed.error().message}
    );
}
}  // namespace

Result<MailId> SqliteStore::complete(const FetchedMail& mail, const Result<ParsedMail>& parsed) {
    const SqlContext context{db_, "storage.complete", mail.key.account_id};
    auto valid = validate_completion(context, mail.key, parsed);
    if (!valid) return std::unexpected(valid.error());

    auto transaction = context.begin(store_detail::TransactionMode::write);
    if (!transaction) return std::unexpected(transaction.error());

    auto current = require_current_key(mail.key, context);
    if (!current) return std::unexpected(current.error());

    auto saved = save_message(context, mail, parsed);
    if (!saved) return std::unexpected(saved.error());

    auto attachments = replace_attachments(context, static_cast<std::int64_t>(*saved), parsed ? &*parsed : nullptr);
    if (!attachments) return std::unexpected(attachments.error());

    auto pending = update_pending_after_parse(context, mail.key, parsed);
    if (!pending) return std::unexpected(pending.error());

    auto committed = context.commit(*transaction);
    if (!committed) return std::unexpected(committed.error());

    return *saved;
}

Status SqliteStore::fail(const RemoteKey& key, const Error& error) {
    const SqlContext context{db_, "storage.fail", key.account_id};
    const auto name = error_name(error.code);
    if (!valid_key(key) || name.empty() || error.message.empty()) {
        return context.fail(ErrorCode::invalid_argument, "Invalid failure report");
    }

    auto transaction = context.begin(store_detail::TransactionMode::write);
    if (!transaction) return std::unexpected(transaction.error());

    auto current = require_current_key(key, context);
    if (!current) return current;

    constexpr const char* sql = R"sql(
        UPDATE sync_pending SET
            retryable = CASE WHEN ?5='remote_gone' THEN 0 ELSE retryable END,
            last_error_code = ?5, last_error_stage = ?6, last_error_message = ?7
        WHERE account_id=?1 AND folder=?2 AND uid_validity=?3 AND uid=?4
    )sql";
    auto updated = context.execute(sql,
        {key.account_id, key.folder, key.uid_validity, key.uid, name, error.stage, error.message});
    if (!updated) return updated;

    if (context.changes() == 0) return context.fail(ErrorCode::invalid_argument, "Pending job was not found");
    return context.commit(*transaction);
}

Status SqliteStore::record_outcome(
    const AccountId& account_id,
    std::string_view folder,
    const Result<SyncSummary>& outcome,
    std::int64_t attempted_at
) {
    const SqlContext context{db_, "storage.outcome", account_id};
    if (account_id.empty() || folder.empty()) {
        return context.fail(ErrorCode::invalid_argument, "Account and folder must not be empty");
    }

    auto transaction = context.begin(store_detail::TransactionMode::write);
    if (!transaction) return std::unexpected(transaction.error());

    auto current = store_detail::load_sync_state(context, account_id, folder);
    if (!current) return std::unexpected(current.error());

    constexpr const char* sql = R"sql(
        INSERT INTO sync_state(account_id, folder, last_attempt_at, last_success_at)
        VALUES (?1, ?2, ?3, CASE WHEN ?4 THEN ?3 ELSE NULL END)
        ON CONFLICT(account_id, folder) DO UPDATE SET
            last_attempt_at = excluded.last_attempt_at,
            last_success_at = CASE WHEN ?4 THEN excluded.last_success_at ELSE sync_state.last_success_at END
    )sql";
    auto updated =
        context.execute(sql, {account_id, folder, attempted_at, outcome && outcome->failed == 0});
    if (!updated) return updated;
    return context.commit(*transaction);
}

}  // namespace mailai

#include "sqlite_rows.h"

namespace mailai::store_detail {
namespace {
Result<ParseState> decode_parse_state(const SqlRow& row, std::string_view state) {
    if (state == "parsed") return ParseState::parsed;
    if (state == "failed") return ParseState::failed;
    return row.fail("Invalid stored parse state");
}
}  // namespace

Result<InboxItem> decode_inbox_item(const SqlRow& row) {
    InboxItem item;
    std::string state;
    auto decoded = row.read(
        item.id,
        item.account_id,
        item.account_name,
        item.sender,
        item.subject,
        item.received_at_utc,
        state,
        item.historical
    );
    if (!decoded) return std::unexpected(decoded.error());
    if (item.id == 0) return row.fail("Invalid stored mail ID");

    auto parsed = decode_parse_state(row, state);
    if (!parsed) return std::unexpected(parsed.error());
    item.parse_state = *parsed;
    return item;
}

Result<Mail> decode_mail(const SqlRow& row) {
    Mail mail;
    std::string state;
    auto decoded = row.read(
        mail.account_id,
        mail.sender,
        mail.subject,
        mail.body,
        mail.received_at_utc,
        mail.folder,
        mail.uid_validity,
        mail.uid,
        state,
        mail.parse_error,
        mail.historical,
        mail.message_id,
        mail.in_reply_to,
        mail.references
    );
    if (!decoded) return std::unexpected(decoded.error());
    if (mail.uid_validity == 0 || mail.uid == 0) return row.fail("Invalid stored remote mail key");

    auto parsed = decode_parse_state(row, state);
    if (!parsed) return std::unexpected(parsed.error());
    mail.parse_state = *parsed;
    return mail;
}

Result<AttachmentInfo> decode_attachment(const SqlRow& row) {
    AttachmentInfo attachment;
    auto decoded = row.read(attachment.name, attachment.media_type, attachment.size_bytes);
    if (!decoded) return std::unexpected(decoded.error());
    return attachment;
}

Result<SyncState> decode_sync_state(const SqlRow& row) {
    SyncState state;
    auto decoded = row.read(
        state.uid_validity,
        state.last_discovered_uid,
        state.last_attempt_at,
        state.last_success_at
    );
    if (!decoded) return std::unexpected(decoded.error());
    if ((state.uid_validity && *state.uid_validity == 0) ||
        (!state.uid_validity && state.last_discovered_uid != 0)) {
        return row.fail("Invalid stored sync state");
    }
    return state;
}

}  // namespace mailai::store_detail

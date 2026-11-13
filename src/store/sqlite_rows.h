#pragma once

#include "mailai/inbox.h"
#include "mailai/sync_types.h"
#include "sqlite_utils.h"

namespace mailai::store_detail {

Result<InboxItem> decode_inbox_item(const SqlRow& row);
Result<Mail> decode_mail(const SqlRow& row);
Result<AttachmentInfo> decode_attachment(const SqlRow& row);
Result<SyncState> decode_sync_state(const SqlRow& row);

}  // namespace mailai::store_detail

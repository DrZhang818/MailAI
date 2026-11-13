#pragma once

#include "mailai/sync_types.h"
#include "sqlite_utils.h"

namespace mailai::store_detail {

// Uses the caller's operation context and any transaction already in progress.
Result<SyncState> load_sync_state(
    const SqlContext& context, const AccountId& account_id, std::string_view folder
);

}  // namespace mailai::store_detail

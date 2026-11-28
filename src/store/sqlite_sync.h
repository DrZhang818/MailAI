#pragma once

#include "mailai/sync_types.h"
#include "sqlite_utils.h"

namespace mailai::store_detail {

// 沿用调用方的操作上下文和已经开启的事务。
Result<SyncState> load_sync_state(
    const SqlContext& context, const AccountId& account_id, std::string_view folder
);

}  // namespace mailai::store_detail

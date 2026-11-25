#pragma once

#include "mailai/config.h"
#include "mailai/sync_types.h"

namespace mailai {

class SqliteStore;

// 调用方负责持有数据目录写锁，并提供已打开的可写存储。
// 单封失败随本轮统计返回；无法继续同步时返回错误。
Result<SyncSummary> sync_inbox(SqliteStore& store, const RuntimeConfig& config);

}  // 命名空间 mailai

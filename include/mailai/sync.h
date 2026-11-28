#pragma once

#include <cstddef>

#include "mailai/config.h"
#include "mailai/sync_types.h"

namespace mailai {

class SqliteStore;

// 调用方负责持有数据目录写锁，并提供已打开的可写存储。
// 单封失败随本轮统计返回；无法继续同步时返回错误。
// history_limit 表示正常同步之外最多补收的历史邮件数量，默认为 0。
Result<SyncSummary> sync_inbox(
    SqliteStore& store, const RuntimeConfig& config, std::size_t history_limit = 0);

}  // namespace mailai

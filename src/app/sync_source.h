#pragma once

#include <cstdint>
#include <functional>

#include "mailai/sync.h"

namespace mailai::sync_detail {

// 只替换远端邮件源，业务流程、MIME 解析和持久化共用真实实现。
struct MailSource {
    std::function<Result<MailboxSnapshot>(const RuntimeConfig&)> read_snapshot;
    std::function<Result<FetchedMail>(const RuntimeConfig&, const RemoteKey&)> fetch;
};

Result<SyncSummary> sync_inbox(SqliteStore& store,
    const RuntimeConfig& config,
    const MailSource& source,
    std::int64_t attempted_at);

}  // 命名空间 mailai::sync_detail

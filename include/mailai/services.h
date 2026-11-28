#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "mailai/config.h"
#include "mailai/inbox.h"
#include "mailai/sync_types.h"

namespace mailai {

enum class DataSource { cache, demo };

// 服务的构造不访问文件或网络，实际资源在调用时取得。
struct MailServices {
    std::function<Result<std::vector<InboxItem>>(DataSource, const InboxQuery&)> list;
    std::function<Result<Mail>(DataSource, MailId)> find;
    std::function<Result<SyncSummary>(std::size_t)> sync;
    std::function<Result<Environment>()> login_defaults{};
    std::function<Status(const Environment&, std::string_view)> login{};
    std::function<Status()> logout{};
};

MailServices make_runtime_services(Environment env);

}  // namespace mailai

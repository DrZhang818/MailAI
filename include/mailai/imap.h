#pragma once

#include "mailai/config.h"
#include "mailai/sync_types.h"

namespace mailai {

// 读取 INBOX 的 UIDVALIDITY 和 UID 列表，不修改邮件标记。
// 临时网络故障最多重试两次，每次重新连接并使用配置的超时。
Result<MailboxSnapshot> read_mailbox_snapshot(const RuntimeConfig& config);

// 下载指定的 INBOX 邮件及其 INTERNALDATE，不改变已读状态。
// 临时故障最多重试两次，重连后重新核对 UIDVALIDITY。
Result<FetchedMail> fetch_mail(const RuntimeConfig& config, const RemoteKey& key);

}  // 命名空间 mailai

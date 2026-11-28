#pragma once

#include <memory>

#include "mailai/config.h"
#include "mailai/sync_types.h"

namespace mailai {

// 在单轮同步中复用同一 IMAP 连接，仅在临时网络故障或状态失效时重连。
class ImapSession {
public:
    explicit ImapSession(RuntimeConfig config);
    ~ImapSession();

    ImapSession(ImapSession&& other) noexcept;
    ImapSession& operator=(ImapSession&& other) noexcept;
    ImapSession(const ImapSession&) = delete;
    ImapSession& operator=(const ImapSession&) = delete;

    // 读取 INBOX 的 UIDVALIDITY 和 UID 列表，不修改邮件标记。
    Result<MailboxSnapshot> read_mailbox_snapshot();

    // 下载指定的 INBOX 邮件及其 INTERNALDATE，不改变已读状态。
    Result<FetchedMail> fetch_mail(const RemoteKey& key);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// 读取 INBOX 的 UIDVALIDITY 和 UID 列表，不修改邮件标记。
// 临时网络故障最多重试两次，每次重新连接并使用配置的超时。
Result<MailboxSnapshot> read_mailbox_snapshot(const RuntimeConfig& config);

// 下载指定的 INBOX 邮件及其 INTERNALDATE，不改变已读状态。
// 临时故障最多重试两次，重连后重新核对 UIDVALIDITY。
Result<FetchedMail> fetch_mail(const RuntimeConfig& config, const RemoteKey& key);

}  // namespace mailai

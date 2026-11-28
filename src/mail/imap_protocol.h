#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mailai/sync_types.h"

namespace mailai::imap_detail {

// 解析选择 INBOX 和查询 UID 时收到的协议响应。
Result<MailboxSnapshot> parse_mailbox_snapshot(
    std::string_view response, const AccountId& account_id);

// 将不含外层引号的 INTERNALDATE 转换为 UTC 时间戳（秒）。
Result<std::int64_t> parse_internal_date(std::string_view date, const AccountId& account_id);

// 判断协议响应是否明确报告临时服务不可用。
bool has_service_unavailable(std::string_view response);

// 收集两路 FETCH 数据并校验协议。该组件不依赖网络库。
class FetchTransfer {
public:
    explicit FetchTransfer(const RemoteKey& key, std::uint32_t inherited_validity = 0);

    // 原文前缀之前每次接收一条完整协议行，之后可接收任意分片。
    bool append_header(std::string_view data);
    bool append_body(std::string_view data);
    void abort() noexcept;

    // 优先报告协议已明确判定的错误。新连接必须在本次响应中提供 UIDVALIDITY。
    Result<FetchedMail> finish(std::optional<Error> transport_error = std::nullopt,
        bool require_observed_validity = false) const;

private:
    bool fail(const char* message, ErrorCode code = ErrorCode::protocol_error) noexcept;
    bool append(std::string& buffer, std::string_view data);

    RemoteKey key;
    std::uint32_t observed_validity{};
    std::uint32_t inherited_validity{};
    bool target_without_body{};
    std::optional<std::size_t> prefix_offset;
    std::size_t literal_offset{};
    std::size_t literal_size{};
    std::string headers;
    std::string payload;
    ErrorCode error_code{ErrorCode::protocol_error};
    const char* error{};
};

}  // namespace mailai::imap_detail

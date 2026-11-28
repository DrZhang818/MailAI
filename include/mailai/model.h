#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mailai {

using AccountId = std::string;
using MailId = std::uint64_t;

struct Account {
    AccountId id;
    std::string display_name;
    std::string address;
};

enum class ParseState { parsed, failed };

struct AttachmentInfo {
    std::string name;
    std::string media_type;
    std::uint64_t size_bytes{};
};

struct BodyTable {
    // 跨行单元格在后续行重复内容，跨列单元格的后续列留空。
    std::vector<std::vector<std::string>> rows;
};

using BodyBlock = std::variant<std::string, BodyTable>;

struct Mail {
    MailId id{};
    AccountId account_id;
    std::string sender;
    std::string subject;
    std::string body;
    std::int64_t received_at_utc{};

    std::string folder{};
    std::uint32_t uid_validity{};
    std::uint32_t uid{};

    ParseState parse_state{ParseState::parsed};
    std::string parse_error{};
    bool historical{false};

    std::string message_id{};
    std::string in_reply_to{};
    std::string references{};
    std::vector<AttachmentInfo> attachments{};
    std::vector<BodyBlock> body_blocks{};
};

enum class ErrorCode {
    unknown_account,
    mail_not_found,
    invalid_argument,
    config_error,
    auth_error,
    network_error,
    protocol_error,
    remote_gone,
    parse_error,
    storage_error,
    busy
};

constexpr std::string_view error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::unknown_account:
            return "unknown_account";
        case ErrorCode::mail_not_found:
            return "mail_not_found";
        case ErrorCode::invalid_argument:
            return "invalid_argument";
        case ErrorCode::config_error:
            return "config_error";
        case ErrorCode::auth_error:
            return "auth_error";
        case ErrorCode::network_error:
            return "network_error";
        case ErrorCode::protocol_error:
            return "protocol_error";
        case ErrorCode::remote_gone:
            return "remote_gone";
        case ErrorCode::parse_error:
            return "parse_error";
        case ErrorCode::storage_error:
            return "storage_error";
        case ErrorCode::busy:
            return "busy";
    }
    return {};
}

struct Error {
    ErrorCode code;
    std::string message;
    std::optional<AccountId> account_id;
    std::optional<MailId> mail_id;
    std::string stage{};
};

template <class T>
using Result = std::expected<T, Error>;

using Status = Result<void>;

}  // namespace mailai

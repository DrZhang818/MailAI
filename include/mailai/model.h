#pragma once

#include <cstdint>
#include <optional>
#include <string>
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

enum class ParseState {
    parsed,
    failed
};

struct AttachmentInfo {
    std::string name;
    std::string media_type;
    std::uint64_t size_bytes{};
};

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
};

enum class ErrorCode {
    unknown_account,
    mail_not_found,
    invalid_argument,
    config_error
};

struct Error {
    ErrorCode code;
    std::string message;
    std::optional<AccountId> account_id;
    std::optional<MailId> mail_id;
};

template <class T>
using Result = std::variant<T, Error>;

} // namespace mailai
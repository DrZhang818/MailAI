#pragma once

#include "mailai/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mailai {

struct RemoteKey {
    AccountId account_id;
    std::string folder;
    std::uint32_t uid_validity{};
    std::uint32_t uid{};
};

struct MailboxSnapshot {
    std::uint32_t uid_validity{};
    std::vector<std::uint32_t> uids;
};

struct FetchedMail {
    RemoteKey key;
    std::int64_t received_at_utc{};
    std::string raw;
};

} // namespace mailai
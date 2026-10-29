#pragma once

#include "mailai/model.h"

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <cstddef>

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

struct SyncState {
    std::optional<std::uint32_t> uid_validity;
    std::uint32_t last_discovered_uid{};
    std::optional<std::int64_t> last_attempt_at;
    std::optional<std::int64_t> last_success_at;
};

struct SyncFailure {
    RemoteKey key;
    Error error;
};

struct SyncSummary {
    std::size_t discovered{};
    std::size_t success{};
    std::size_t failed{};
    bool reindexed{};
    std::vector<SyncFailure> failures;
};

} // namespace mailai

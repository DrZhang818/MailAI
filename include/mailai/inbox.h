#pragma once

#include "mailai/model.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mailai {

struct InboxQuery {
    std::optional<AccountId> account_id;
};

struct InboxItem {
    MailId id{};
    AccountId account_id;
    std::string account_name;
    std::string sender;
    std::string subject;
    std::int64_t received_at_utc{};
    ParseState parse_state{ParseState::parsed};
    bool historical{false};
};

Result<std::vector<InboxItem>> list_inbox(
    const std::vector<Account>& accounts,
    const std::vector<Mail>& mails,
    const InboxQuery& query
);

Result<Mail> find_mail(
    const std::vector<Mail>& mails,
    MailId id
);

} // namespace mailai
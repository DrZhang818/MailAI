#include "mailai/inbox.h"

#include <algorithm>

#include "mailai/error_message.h"

namespace mailai {

namespace {

bool contains_keyword(std::string_view text, std::string_view keyword) {
    const auto fold = [](unsigned char ch) { return ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch; };
    return std::ranges::search(text, keyword, [&](unsigned char a, unsigned char b) {
        return fold(a) == fold(b);
    }).begin() != text.end();
}

bool matches(const Mail& mail, const InboxQuery& query) {
    if (query.account_id && mail.account_id != query.account_id) {
        return false;
    }

    return !query.keyword || query.keyword->empty() ||
           contains_keyword(mail.subject, *query.keyword) ||
           contains_keyword(mail.sender, *query.keyword) ||
           contains_keyword(mail.body, *query.keyword);
}

}  // namespace

Result<std::vector<InboxItem>> list_inbox(
    const std::vector<Account>& accounts, const std::vector<Mail>& mails, const InboxQuery& query) {
    if (query.account_id) {
        const auto account = std::ranges::find(accounts, *query.account_id, &Account::id);
        if (account == accounts.end()) {
            return std::unexpected(Error{
                ErrorCode::unknown_account,
                error_message::common::unknown_account,
                query.account_id,
                std::nullopt
            });
        }
    }

    std::vector<InboxItem> items;

    for (const auto& mail : mails) {
        if (!matches(mail, query)) {
            continue;
        }
        const auto account = std::ranges::find(accounts, mail.account_id, &Account::id);
        if (account == accounts.end()) {
            return std::unexpected(Error{
                ErrorCode::unknown_account,
                error_message::common::mail_account_missing,
                mail.account_id,
                mail.id
            });
        }
        items.push_back(InboxItem{
            mail.id,
            mail.account_id,
            account->display_name,
            mail.sender,
            mail.subject,
            mail.received_at_utc,
            mail.parse_state,
            mail.historical
        });
    }

    std::ranges::sort(items, [](const InboxItem& a, const InboxItem& b) {
        if (a.received_at_utc != b.received_at_utc) {
            return a.received_at_utc > b.received_at_utc;
        }
        return a.id < b.id;
    });

    return items;
}

Result<Mail> find_mail(const std::vector<Mail>& mails, MailId id) {
    if (id == 0) {
        return std::unexpected(Error{
            ErrorCode::invalid_argument, error_message::common::invalid_mail_id, std::nullopt, id
        });
    }

    const auto mail = std::ranges::find(mails, id, &Mail::id);

    if (mail == mails.end()) {
        return std::unexpected(Error{
            ErrorCode::mail_not_found, error_message::common::mail_not_found, std::nullopt, id
        });
    }

    return *mail;
}

}  // namespace mailai

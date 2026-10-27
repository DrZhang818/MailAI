#include "mailai/inbox.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using namespace mailai;

const std::vector<Account> accounts{
    {"school", "学校邮箱", "student@school.example"},
    {"qq", "QQ邮箱", "student@qq.example"}
};

const std::vector<Mail> mails{
    {44, "school", "library@school.example",
     "图书馆通知", "请按时归还图书", 100},
    {43, "qq", "recruiter@example.com",
     "实习通知", "请确认面试时间", 200},
    {42, "school", "course@school.example",
     "课程通知", "请查看课程安排", 200}
};

void test_inbox_order()
{
    const auto all = list_inbox(accounts, mails, InboxQuery{}).value();

    expect(all.size() == 3, "all count");
    expect(all[0].id == 42 && all[1].id == 43 && all[2].id == 44,
           "stable order");

    expect(all[0].account_id == "school"
               && all[0].account_name == "学校邮箱",
           "account association for 42");
    expect(all[1].account_id == "qq"
               && all[1].account_name == "QQ邮箱",
           "account association for 43");
    expect(all[2].account_id == "school"
               && all[2].account_name == "学校邮箱",
           "account association for 44");
}

void test_account_filter()
{
    const auto school = list_inbox(
        accounts, mails, InboxQuery{AccountId{"school"}}
    ).value();

    expect(school.size() == 2, "school count");
    expect(school[0].id == 42 && school[1].id == 44, "account filter");
}

void test_inbox_order_is_independent_of_input()
{
    auto input = mails;
    input.push_back(Mail{
        7, "school", "older@school.example",
        "Older message", "Older body", 50
    });

    std::array order{0, 1, 2, 3};
    do {
        std::vector<Mail> permuted;
        for (const auto index : order) {
            permuted.push_back(input[index]);
        }

        const auto result = list_inbox(accounts, permuted, InboxQuery{});
        expect(result.has_value(), "each inbox input permutation should succeed");
        expect(result->size() == 4, "sorting should preserve all four messages");
        expect((*result)[0].id == 42 && (*result)[1].id == 43
                   && (*result)[2].id == 44 && (*result)[3].id == 7,
               "inbox should sort by descending time then ascending ID for every input permutation");
    } while (std::next_permutation(order.begin(), order.end()));
}

void test_inbox_preserves_message_state()
{
    auto stateful_mails = mails;
    stateful_mails[0].parse_state = ParseState::failed;
    stateful_mails[0].historical = true;
    stateful_mails[1].historical = true;
    stateful_mails[2].parse_state = ParseState::failed;

    const auto result = list_inbox(accounts, stateful_mails, InboxQuery{});
    expect(result.has_value(),
           "inbox state query should succeed");

    const auto& items = *result;
    expect(items.size() == 3, "inbox state query should preserve all records");
    expect(items[0].id == 42 && items[1].id == 43 && items[2].id == 44,
           "inbox states should remain associated with stable mail IDs after sorting");

    expect(items[0].parse_state == ParseState::failed,
           "inbox should preserve a failed parse state");
    expect(!items[0].historical,
           "a current record should remain current even when parsing failed");

    expect(items[1].parse_state == ParseState::parsed,
           "inbox should preserve a successful parse state");
    expect(items[1].historical,
           "a historical record should remain historical even when parsing succeeded");

    expect(items[2].parse_state == ParseState::failed,
           "inbox should preserve parse failure on a historical record");
    expect(items[2].historical,
           "inbox should preserve historical status on a failed record");
}

void test_find_mail_by_id()
{
    const auto mail = find_mail(mails, 43).value();

    expect(mail.id == 43, "stable mail id");
    expect(mail.account_id == "qq", "mail account");
    expect(mail.subject == "实习通知", "mail subject");
}

void test_empty_inbox()
{
    const auto school = list_inbox(
        accounts, {}, InboxQuery{AccountId{"school"}}
    ).value();
    expect(school.empty(), "known account with no mail");

    const auto all = list_inbox({}, {}, InboxQuery{}).value();
    expect(all.empty(), "no accounts and no mail");
}

void test_find_mail_with_uint64_max_id()
{
    constexpr MailId large_id = std::numeric_limits<std::uint64_t>::max();
    const std::vector<Mail> large_id_mails{
        {large_id, "school", "course@school.example",
         "大 ID 邮件", "测试正文", 200}
    };

    const auto mail = find_mail(large_id_mails, large_id).value();

    expect(mail.id == large_id, "uint64 max mail id");
    expect(mail.account_id == "school" && mail.subject == "大 ID 邮件",
           "uint64 max mail data");
}

void test_unknown_account()
{
    const auto result = list_inbox(
        accounts, mails, InboxQuery{AccountId{"missing"}}
    );
    expect(!result.has_value(), "unknown account should return an error");
    const auto& error = result.error();

    expect(error.code == ErrorCode::unknown_account, "unknown account error");
    expect(error.account_id == AccountId{"missing"}, "unknown account context");

    const auto empty_result = list_inbox(
        accounts, {}, InboxQuery{AccountId{"missing"}}
    );
    expect(!empty_result.has_value(),
           "unknown account with no mail should return an error");
    const auto& empty_error = empty_result.error();
    expect(empty_error.code == ErrorCode::unknown_account,
           "unknown account with no mail");
}

void test_mail_referencing_unknown_account()
{
    const std::vector<Mail> invalid_mails{
        {45, "missing", "sender@example.com", "测试邮件", "正文", 300}
    };
    const auto result = list_inbox(accounts, invalid_mails, InboxQuery{});
    expect(!result.has_value(),
           "mail referencing an unknown account should return an error");
    const auto& error = result.error();

    expect(error.code == ErrorCode::unknown_account,
           "mail references unknown account");
    expect(error.account_id == AccountId{"missing"}
               && error.mail_id == MailId{45},
           "unknown mail account context");
}

void test_mail_not_found()
{
    const auto result = find_mail(mails, 999);
    expect(!result.has_value(), "missing mail should return an error");
    const auto& error = result.error();

    expect(error.code == ErrorCode::mail_not_found, "mail not found error");
    expect(error.mail_id == MailId{999}, "missing mail context");
}

void test_invalid_mail_id()
{
    const auto result = find_mail(mails, 0);
    expect(!result.has_value(), "invalid mail ID should return an error");
    const auto& error = result.error();

    expect(error.code == ErrorCode::invalid_argument, "invalid mail id");
}

} // namespace

int main()
{
    try {
        test_inbox_order();
        test_inbox_order_is_independent_of_input();
        test_account_filter();
        test_inbox_preserves_message_state();
        test_find_mail_by_id();
        test_find_mail_with_uint64_max_id();
        test_empty_inbox();
        test_unknown_account();
        test_mail_referencing_unknown_account();
        test_mail_not_found();
        test_invalid_mail_id();

        std::cout << "core query tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}

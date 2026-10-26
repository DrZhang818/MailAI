#include "mailai/inbox.h"
#include "test_support.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <variant>
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
    const auto all = std::get<std::vector<InboxItem>>(
        list_inbox(accounts, mails, InboxQuery{})
    );

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
    const auto school = std::get<std::vector<InboxItem>>(
        list_inbox(accounts, mails, InboxQuery{AccountId{"school"}})
    );

    expect(school.size() == 2, "school count");
    expect(school[0].id == 42 && school[1].id == 44, "account filter");
}

void test_inbox_preserves_message_state()
{
    auto stateful_mails = mails;
    stateful_mails[0].parse_state = ParseState::failed;
    stateful_mails[0].historical = true;
    stateful_mails[1].historical = true;
    stateful_mails[2].parse_state = ParseState::failed;

    const auto result = list_inbox(accounts, stateful_mails, InboxQuery{});
    expect(std::holds_alternative<std::vector<InboxItem>>(result),
           "inbox state query should succeed");

    const auto& items = std::get<std::vector<InboxItem>>(result);
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
    const auto mail = std::get<Mail>(find_mail(mails, 43));

    expect(mail.id == 43, "stable mail id");
    expect(mail.account_id == "qq", "mail account");
    expect(mail.subject == "实习通知", "mail subject");
}

void test_empty_inbox()
{
    const auto school = std::get<std::vector<InboxItem>>(
        list_inbox(accounts, {}, InboxQuery{AccountId{"school"}})
    );
    expect(school.empty(), "known account with no mail");

    const auto all = std::get<std::vector<InboxItem>>(
        list_inbox({}, {}, InboxQuery{})
    );
    expect(all.empty(), "no accounts and no mail");
}

void test_find_mail_with_uint64_max_id()
{
    constexpr MailId large_id = std::numeric_limits<std::uint64_t>::max();
    const std::vector<Mail> large_id_mails{
        {large_id, "school", "course@school.example",
         "大 ID 邮件", "测试正文", 200}
    };

    const auto mail = std::get<Mail>(find_mail(large_id_mails, large_id));

    expect(mail.id == large_id, "uint64 max mail id");
    expect(mail.account_id == "school" && mail.subject == "大 ID 邮件",
           "uint64 max mail data");
}

void test_unknown_account()
{
    const auto error = std::get<Error>(
        list_inbox(accounts, mails, InboxQuery{AccountId{"missing"}})
    );

    expect(error.code == ErrorCode::unknown_account, "unknown account error");
    expect(error.account_id == AccountId{"missing"}, "unknown account context");

    const auto empty_error = std::get<Error>(
        list_inbox(accounts, {}, InboxQuery{AccountId{"missing"}})
    );
    expect(empty_error.code == ErrorCode::unknown_account,
           "unknown account with no mail");
}

void test_mail_referencing_unknown_account()
{
    const std::vector<Mail> invalid_mails{
        {45, "missing", "sender@example.com", "测试邮件", "正文", 300}
    };
    const auto error = std::get<Error>(
        list_inbox(accounts, invalid_mails, InboxQuery{})
    );

    expect(error.code == ErrorCode::unknown_account,
           "mail references unknown account");
    expect(error.account_id == AccountId{"missing"}
               && error.mail_id == MailId{45},
           "unknown mail account context");
}

void test_mail_not_found()
{
    const auto error = std::get<Error>(find_mail(mails, 999));

    expect(error.code == ErrorCode::mail_not_found, "mail not found error");
    expect(error.mail_id == MailId{999}, "missing mail context");
}

void test_invalid_mail_id()
{
    const auto error = std::get<Error>(find_mail(mails, 0));

    expect(error.code == ErrorCode::invalid_argument, "invalid mail id");
}

} // namespace

int main()
{
    try {
        test_inbox_order();
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

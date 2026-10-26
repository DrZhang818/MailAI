#include "cli.h"
#include "test_support.h"

#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
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

struct CliResult {
    int code;
    std::string out;
    std::string err;
};

CliResult invoke(const std::vector<std::string>& args,
                 const std::vector<Mail>& input_mails = mails)
{
    std::ostringstream out;
    std::ostringstream err;
    const int code = cli::run_cli(args, accounts, input_mails, out, err);
    return {code, out.str(), err.str()};
}

bool contains(const std::string& text, std::string_view fragment)
{
    return text.find(fragment) != std::string::npos;
}

void expect_success(const CliResult& result)
{
    expect(result.code == 0, "success exit code");
    expect(result.err.empty(), "success leaves stderr empty");
}

void expect_failure(const CliResult& result, int code)
{
    expect(result.code == code, "failure exit code");
    expect(result.out.empty(), "failure leaves stdout empty");
    expect(!result.err.empty(), "failure explains the reason on stderr");
}

void test_help()
{
    const std::vector<std::vector<std::string>> cases{
        {}, {"--help"}, {"inbox", "--help"}, {"show", "--help"}
    };

    for (const auto& args : cases) {
        const auto result = invoke(args);
        expect_success(result);
        expect(contains(result.out, "Usage:"), "help usage");
        expect(contains(result.out, "inbox"), "help inbox command");
        expect(contains(result.out, "show"), "help show command");
    }
}

void test_inbox_all()
{
    const std::vector<std::vector<std::string>> cases{
        {"inbox"}, {"inbox", "--account", "all"}
    };

    for (const auto& args : cases) {
        const auto result = invoke(args);
        expect_success(result);
        for (const auto* fragment : {
                 "数据来源：示例", "42", "43", "44", "学校邮箱", "QQ邮箱",
                 "课程通知", "实习通知", "图书馆通知", "3 封",
                 "course@school.example", "recruiter@example.com",
                 "library@school.example"}) {
            const std::string message =
                "inbox content missing: " + std::string(fragment);
            expect(contains(result.out, fragment), message.c_str());
        }
        expect(result.out.find("42") < result.out.find("43")
                   && result.out.find("43") < result.out.find("44"),
               "display preserves inbox order");
    }
}

void test_account_filter()
{
    const auto result = invoke({"inbox", "--account", "school"});
    expect_success(result);
    expect(contains(result.out, "课程通知"), "school course mail");
    expect(contains(result.out, "图书馆通知"), "school library mail");
    expect(!contains(result.out, "实习通知"), "filter excludes qq mail");
    expect(contains(result.out, "学校邮箱"), "filtered account name");
    expect(contains(result.out, "2 封"), "filtered mail count");
}

void test_empty_inbox()
{
    const auto result = invoke({"inbox"}, {});
    expect_success(result);
    expect(contains(result.out, "0 封"), "empty inbox count");
    expect(contains(result.out, "数据来源：示例"), "empty inbox sample label");
}

void test_show_by_stable_id()
{
    const auto result = invoke({"show", "43"});
    expect_success(result);
    for (const auto* fragment : {
             "数据来源：示例", "43", "qq", "实习通知", "请确认面试时间",
             "recruiter@example.com"}) {
        expect(contains(result.out, fragment), "mail detail content");
    }
}

void test_runtime_errors()
{
    struct Case {
        std::vector<std::string> args;
        const char* context;
    };
    const std::vector<Case> cases{
        {{"inbox", "--account", "missing"}, "missing"},
        {{"show", "999"}, "999"},
        {{"show", "18446744073709551615"}, "18446744073709551615"}
    };

    for (const auto& test : cases) {
        const auto result = invoke(test.args);
        expect_failure(result, 1);
        expect(contains(result.err, test.context), "runtime error context");
    }
}

void test_bad_arguments()
{
    const std::vector<std::vector<std::string>> cases{
        {"inbox", "--account"},
        {"inbox", "--account", "school", "--account", "qq"},
        {"show"},
        {"show", "42", "43"},
        {"unknown"},
        {"inbox", "--unexpected"},
        {"inbox", "--account", "missing", "--unexpected"},
        {"show", "999", "extra"},
        {"show", ""},
        {"show", "0"},
        {"show", "-1"},
        {"show", "+42"},
        {"show", "42x"},
        {"show", "18446744073709551616"},
        {"show", "999999999999999999999999"}
    };

    for (const auto& args : cases) {
        const auto result = invoke(args);
        expect_failure(result, 2);
    }
}

void test_account_option_requires_value()
{
    const std::vector<std::vector<std::string>> cases{
        {"inbox", "--account", ""},
        {"inbox", "--account", "--account"},
        {"inbox", "--account", "--unexpected"},
        {"inbox", "--account", "--help"}
    };

    for (const auto& args : cases) {
        const auto result = invoke(args);
        expect(result.code == 2,
               "--account requires a nonempty value, not another option");
        expect(result.out.empty(), "missing account value leaves stdout empty");
        expect(!result.err.empty(), "missing account value has a diagnostic");
    }
}

} // namespace

int main()
{
    try {
        test_help();
        test_inbox_all();
        test_account_filter();
        test_empty_inbox();
        test_show_by_stable_id();
        test_runtime_errors();
        test_bad_arguments();
        test_account_option_requires_value();

        std::cout << "CLI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}

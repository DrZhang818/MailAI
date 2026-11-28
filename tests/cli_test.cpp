#include "cli/cli.h"

#include <algorithm>
#include <exception>
#include <expected>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mailai/services.h"
#include "test_support.h"

namespace {

using namespace mailai;

const std::vector<Account> accounts{
    {"school", "学校邮箱", "student@school.example"}, {"qq", "QQ邮箱", "student@qq.example"}
};

const std::vector<Mail> mails{
    {44, "school", "library@school.example", "图书馆通知", "请按时归还图书", 100},
    {43, "qq", "recruiter@example.com", "实习通知", "请确认面试时间", 200},
    {42, "school", "course@school.example", "课程通知", "请查看课程安排", 200}
};

struct CliResult {
    int code;
    std::string out;
    std::string err;
};

CliResult invoke_with(const std::vector<std::string>& args,
    const MailServices& services,
    cli::TerminalOptions terminal = {},
    const cli::LoginInput& input = {}) {
    std::ostringstream out;
    std::ostringstream err;
    const int code = cli::run_cli(args, services, out, err, terminal, input);
    return {code, out.str(), err.str()};
}

CliResult invoke(
    const std::vector<std::string>& args, const std::vector<Mail>& input_mails = mails) {
    const MailServices services{
        [&](DataSource source, const InboxQuery& query) {
            expect(source == DataSource::demo, "sample query must use demo mode");
            return list_inbox(accounts, input_mails, query);
        },
        [&](DataSource source, MailId id) {
            expect(source == DataSource::demo, "sample detail must use demo mode");
            return find_mail(input_mails, id);
        },
        [](std::size_t) -> Result<SyncSummary> {
            throw std::runtime_error("unexpected synchronization");
        }
    };
    auto explicit_demo = args;
    if (!args.empty())
        explicit_demo.insert(explicit_demo.begin(), "--demo");
    return invoke_with(explicit_demo, services);
}

bool contains(const std::string& text, std::string_view fragment) {
    return text.find(fragment) != std::string::npos;
}

void expect_success(const CliResult& result) {
    expect(result.code == 0, "success exit code");
    expect(result.err.empty(), "success leaves stderr empty");
}

void expect_failure(const CliResult& result, int code) {
    expect(result.code == code, "failure exit code");
    expect(result.out.empty(), "failure leaves stdout empty");
    expect(!result.err.empty(), "failure explains the reason on stderr");
}

void test_help() {
    const std::vector<std::vector<std::string>> cases{
        {}, {"--help"}, {"inbox", "--help"}, {"show", "--help"}
    };

    for (const auto& args : cases) {
        const auto result = invoke(args);
        expect_success(result);
        expect(contains(result.out, "Usage:"), "help usage");
        expect(contains(result.out, "inbox"), "help inbox command");
        expect(contains(result.out, "show"), "help show command");
        expect(contains(result.out, "mail_assistant login"), "help login command");
        expect(contains(result.out, "mail_assistant logout"), "help logout command");
    }
}

void test_inbox_all() {
    const std::vector<std::vector<std::string>> cases{{"inbox"}, {"inbox", "--account", "all"}};

    for (const auto& args : cases) {
        const auto result = invoke(args);
        expect_success(result);
        for (const auto* fragment :
            {"数据来源：示例",
                "42",
                "43",
                "44",
                "学校邮箱",
                "QQ邮箱",
                "课程通知",
                "实习通知",
                "图书馆通知",
                "3 封",
                "course@school.example",
                "recruiter@example.com",
                "library@school.example"}) {
            const std::string message = "inbox content missing: " + std::string(fragment);
            expect(contains(result.out, fragment), message.c_str());
        }
        expect(result.out.find("42") < result.out.find("43") &&
                   result.out.find("43") < result.out.find("44"),
            "display preserves inbox order");
        expect(contains(result.out, "ID   账号       发件人") &&
                   contains(result.out, "42   学校邮箱   course@school.example") &&
                   contains(result.out, "43   QQ邮箱     recruiter@example.com"),
            "columns align by display width rather than UTF-8 bytes");
        expect(contains(result.out, "──") && !contains(result.out, "\t") &&
                   !contains(result.out, "\x1b") && !contains(result.out, " / 当前"),
            "plain inbox has row dividers, no tab or color escapes, and concise status");
    }
}

void test_account_filter() {
    const auto result = invoke({"inbox", "--account", "school"});
    expect_success(result);
    expect(contains(result.out, "课程通知"), "school course mail");
    expect(contains(result.out, "图书馆通知"), "school library mail");
    expect(!contains(result.out, "实习通知"), "filter excludes qq mail");
    expect(contains(result.out, "学校邮箱"), "filtered account name");
    expect(contains(result.out, "2 封"), "filtered mail count");
}

void test_id_order_and_full_subject() {
    const std::string subject = std::string(123, 'Q') + "完整主题";
    const std::vector<Mail> unordered{
        {10, "school", "sender@example.test", subject, "", 300},
        {2, "school", "sender@example.test", "较早邮件", "", 100}
    };
    const auto result = invoke({"inbox"}, unordered);
    expect_success(result);
    expect(result.out.find("\n2 ") < result.out.find("\n10 "), "IDs ascend independently of time");
    expect(std::count(result.out.begin(), result.out.end(), 'Q') == 123 &&
               contains(result.out, "完整主题"),
        "long subject must wrap without dropping any characters");
}

void test_find_command() {
    const auto result = invoke({"find", "面试"});
    expect_success(result);
    expect(contains(result.out, "实习通知") && contains(result.out, "1 封") &&
               !contains(result.out, "课程通知"),
        "find searches the cached body, not just the subject");
    const auto sender = invoke({"find", "RECRUITER"});
    expect_success(sender);
    expect(contains(sender.out, "实习通知") && contains(sender.out, "1 封"),
        "ASCII sender matching ignores letter case");
    expect_failure(invoke({"find", ""}), 2);
    expect_failure(invoke({"find"}), 2);
}

void test_empty_inbox() {
    const auto result = invoke({"inbox"}, {});
    expect_success(result);
    expect(contains(result.out, "0 封"), "empty inbox count");
    expect(contains(result.out, "数据来源：示例"), "empty inbox sample label");
    expect(contains(result.out, "暂无邮件"), "empty inbox does not show an empty grid");
}

void test_show_by_stable_id() {
    const auto result = invoke({"show", "43"});
    expect_success(result);
    for (const auto* fragment :
        {"数据来源：示例", "43", "qq", "实习通知", "请确认面试时间", "recruiter@example.com"}) {
        expect(contains(result.out, fragment), "mail detail content");
    }
    expect(contains(result.out, "状态: 已解析\n") && !contains(result.out, "当前"),
        "normal details need only the parse state");
}

void test_runtime_errors() {
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

void test_bad_arguments() {
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

void test_account_option_requires_value() {
    const std::vector<std::vector<std::string>> cases{
        {"inbox", "--account", ""},
        {"inbox", "--account", "--account"},
        {"inbox", "--account", "--unexpected"},
        {"inbox", "--account", "--help"}
    };

    for (const auto& args : cases) {
        const auto result = invoke(args);
        expect(result.code == 2, "--account requires a nonempty value, not another option");
        expect(result.out.empty(), "missing account value leaves stdout empty");
        expect(!result.err.empty(), "missing account value has a diagnostic");
    }
}

void test_validation_precedes_service_calls() {
    const MailServices unused{
        [](DataSource, const InboxQuery&) -> Result<std::vector<InboxItem>> {
            throw std::runtime_error("unexpected list");
        },
        [](DataSource, MailId) -> Result<Mail> { throw std::runtime_error("unexpected find"); },
        [](std::size_t) -> Result<SyncSummary> { throw std::runtime_error("unexpected sync"); }
    };
    for (const auto& args : std::vector<std::vector<std::string>>{
             {}, {"--help"}, {"sync", "--help"}, {"--demo", "--help"}
         }) {
        const auto result = invoke_with(args, unused);
        expect_success(result);
        expect(contains(result.out, "sync") && contains(result.out, "--demo"), "updated help");
    }
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"--demo"},
             {"--demo", "sync"},
             {"sync", "extra"},
             {"sync", "--history", "0"},
             {"sync", "--history", "-3"},
             {"sync", "--history", "3x"},
             {"sync", "--history"},
             {"sync", "--all", "--history", "3"},
             {"--demo", "sync", "--all"},
             {"inbox", "--demo"},
             {"--demo", "--demo", "inbox"},
             {"show", "18446744073709551616"}
         }) {
        expect_failure(invoke_with(args, unused), 2);
    }
}

void test_cache_and_terminal_display() {
    const auto controls = std::string("A\x1b[31m\tB\nC\rD\x7f") + "\xc2\x9b" + "\xe2\x80\xae";
    Mail mail{42, "school", controls, "主题", "第一行\r\n第二行\n第三行\r尾\x1b[0m", 0};
    mail.parse_state = ParseState::failed;
    mail.parse_error = "bad\nparse";
    mail.historical = true;
    mail.attachments.push_back({controls, "application/octet-stream", 9});
    const MailServices services{
        [&](DataSource source, const InboxQuery& query) -> Result<std::vector<InboxItem>> {
            expect(
                source == DataSource::cache && query.account_id == "school", "cache list routing");
            return std::vector<InboxItem>{
                {42, "school", "学校邮箱", mail.sender, mail.subject, 0, ParseState::failed, true}
            };
        },
        [&](DataSource source, MailId id) -> Result<Mail> {
            expect(source == DataSource::cache && id == 42, "cache detail routing");
            return mail;
        },
        [](std::size_t) -> Result<SyncSummary> { throw std::runtime_error("unexpected sync"); }
    };
    const auto list = invoke_with({"inbox", "--account", "school"}, services);
    const auto detail = invoke_with({"show", "42"}, services);
    for (const auto* result : {&list, &detail}) {
        expect_success(*result);
        expect(contains(result->out, "数据来源：缓存") && contains(result->out, "解析失败") &&
                   contains(result->out, "历史邮件"),
            "cache output shows source and states");
        expect(!contains(result->out, "\x1b") && !contains(result->out, "\x7f") &&
                   !contains(result->out, "\xc2\x9b") && !contains(result->out, "\xe2\x80\xae") &&
                   !contains(result->out, "B\nC") && !contains(result->out, "\r"),
            "untrusted metadata must not emit terminal control characters");
        expect(contains(result->out, "主题"), "valid UTF-8 stays readable");
    }
    expect(contains(detail.out, "第一行\n第二行\n第三行") &&
               contains(detail.out, "application/octet-stream") && contains(detail.out, "9") &&
               contains(detail.out, "bad") && contains(detail.out, "parse"),
        "detail preserves body line breaks, attachments and parsing diagnostics");
}

void test_narrow_and_styled_table() {
    const auto max_id = std::numeric_limits<MailId>::max();
    Mail mail{max_id, "school", std::string(180, 'x'), std::string(180, 'y'), "正文", 100};
    mail.historical = true;
    const MailServices services{
        [&](DataSource, const InboxQuery&) -> Result<std::vector<InboxItem>> {
            return std::vector<InboxItem>{
                {max_id,
                    "school",
                    "学校邮箱",
                    mail.sender,
                    mail.subject,
                    mail.received_at_utc,
                    mail.parse_state,
                    mail.historical}
            };
        },
        [&](DataSource, MailId) -> Result<Mail> { return mail; },
        [](std::size_t) -> Result<SyncSummary> { throw std::runtime_error("unexpected sync"); }
    };
    const auto narrow = invoke_with({"inbox"}, services, {84, false});
    expect_success(narrow);
    expect(contains(narrow.out, "18446744073709551615") && contains(narrow.out, "历史邮件") &&
               contains(narrow.out, "…") && !contains(narrow.out, mail.sender) &&
               !contains(narrow.out, mail.subject),
        "narrow tables shorten long metadata without losing the full local ID and status");
    expect(std::count(narrow.out.begin(), narrow.out.end(), 'y') == 180,
        "narrow terminals wrap all subject characters");
    // 此夹具仅含 ASCII、汉字和单列宽的表格分隔符。
    std::istringstream lines(narrow.out);
    for (std::string line; std::getline(lines, line);) {
        std::size_t width{};
        for (std::string_view rest{line}; !rest.empty();) {
            if (static_cast<unsigned char>(rest.front()) < 0x80) {
                ++width;
                rest.remove_prefix(1);
            } else {
                expect(rest.size() >= 3, "table preserves complete UTF-8 characters");
                width += rest.starts_with("─") || rest.starts_with("…") ? 1 : 2;
                rest.remove_prefix(3);
            }
        }
        expect(width <= 84, "narrow table fits within the available display columns");
    }
    const auto very_narrow = invoke_with({"inbox"}, services, {40, false});
    expect_success(very_narrow);
    expect(contains(very_narrow.out, "ID: 18446744073709551615") &&
               contains(very_narrow.out, "状态: 已解析 / 历史邮件"),
        "terminals too narrow for a table fall back to a readable record layout");
    expect(std::count(very_narrow.out.begin(), very_narrow.out.end(), 'y') == 180,
        "stacked layout preserves the whole subject");
    const auto tiny = invoke_with({"inbox"}, services, {20, false});
    expect_success(tiny);
    expect(contains(tiny.out, "ID:\n18446744073709551615\n") &&
               contains(tiny.out, "状态:\n已解析 / 历史邮件\n"),
        "fixed values keep all characters on their own line when labels cannot fit");
    const auto detail = invoke_with({"show", "18446744073709551615"}, services, {84, false});
    expect_success(detail);
    expect(contains(detail.out, mail.sender) && contains(detail.out, mail.subject),
        "detail retains full content regardless of terminal width");
    const auto styled = invoke_with({"inbox"}, services, {120, true});
    expect_success(styled);
    expect(contains(styled.out, "\x1b[1;33m") && contains(styled.out, "\x1b[0m"),
        "interactive output styles table headers");
}

void test_sync_exit_codes() {
    MailServices services;
    const Error failure{
        ErrorCode::network_error, "Synthetic failure\x1b", "school", std::nullopt, "imap.fetch"
    };
    const SyncFailure item{{"school", "INBOX", 77, 102}, failure};
    const std::vector<std::pair<SyncSummary, int>> cases{
        {{}, 0},
        {{2, 2, 0, false, {}}, 0},
        {{2, 1, 1, true, {item}}, 3},
        {{1, 0, 1, false, {item}}, 1}
    };
    for (const auto& [summary, code] : cases) {
        services.sync = [&](std::size_t limit) {
            expect(limit == 0, "ordinary sync must not backfill history");
            return Result<SyncSummary>{summary};
        };
        const auto result = invoke_with({"sync"}, services);
        expect(result.code == code && contains(result.out, "新增") &&
                   contains(result.out, "成功") && contains(result.out, "失败"),
            "sync reports counts and maps the result to an exit code");
        if (summary.failed) {
            expect(contains(result.err, "102") && contains(result.err, "imap.fetch") &&
                       contains(result.err, "school") && !contains(result.err, "\x1b"),
                "per-mail failure is located and safely displayed");
        } else {
            expect(result.err.empty(), "successful sync has no diagnostic");
        }
        if (summary.reindexed)
            expect(contains(result.out, "重新索引"), "range change is visible");
    }
    services.sync = [&](std::size_t) { return Result<SyncSummary>{std::unexpected(failure)}; };
    expect_failure(invoke_with({"sync"}, services), 1);
}

void test_sync_history_options() {
    MailServices services;
    services.sync = [](std::size_t limit) {
        return Result<SyncSummary>{SyncSummary{limit == 42 ? 42U : 0U, 0, 0, false, {}}};
    };
    const auto history = invoke_with({"sync", "--history", "42"}, services);
    expect_success(history);
    expect(contains(history.out, "新增 42 封"), "history requests the specified count");
    services.sync = [](std::size_t limit) {
        expect(limit == std::numeric_limits<std::size_t>::max(), "all requests unlimited history");
        return Result<SyncSummary>{SyncSummary{}};
    };
    expect_success(invoke_with({"sync", "--all"}, services));
}

void test_login_and_logout_arguments() {
    MailServices services;
    services.login_defaults = []() -> Result<Environment> {
        throw std::runtime_error("unexpected login defaults");
    };
    services.login = [](const Environment&, std::string_view) -> Status {
        throw std::runtime_error("unexpected login");
    };
    services.logout = []() -> Status { throw std::runtime_error("unexpected logout"); };
    for (const auto& command : {"login", "logout"}) {
        const auto help = invoke_with({command, "--help"}, services);
        expect_success(help);
        expect(contains(help.out, "mail_assistant login") &&
                   contains(help.out, "mail_assistant logout"),
            "login and logout are documented");
        expect_failure(invoke_with({"--demo", command}, services), 2);
        expect_failure(invoke_with({command, "extra"}, services), 2);
    }
    const auto invalid = invoke_with({"login", "--password", "sample-secret"}, services);
    expect_failure(invalid, 2);
    expect(!contains(invalid.err, "sample-secret"), "bad arguments never echo password values");
}

void test_login_defaults_and_no_secret_output() {
    Environment captured;
    const std::string secret = " sample-password ";
    int calls{};
    MailServices services;
    services.login_defaults = [] {
        return Result<Environment>{
            {{"HOME", "/test/home"}, {"MAILAI_PASSWORD_ENV", "LEGACY_PASSWORD_REFERENCE"}}
        };
    };
    services.login = [&](const Environment& env, std::string_view password) -> Status {
        ++calls;
        captured = env;
        expect(password == secret, "password reaches service unchanged");
        return {};
    };
    std::istringstream lines("student@bjtu.edu.cn\n\n\n\n\n\n");
    const cli::LoginInput input{
        [&](std::string& line) { return static_cast<bool>(std::getline(lines, line)); },
        [&](std::string& password) {
            password = secret;
            return true;
        }
    };
    auto result = invoke_with({"login"}, services, {}, input);
    expect(result.code == 0 && !result.out.empty() && calls == 1, "login succeeds once");
    expect(captured.at("MAILAI_EMAIL") == "student@bjtu.edu.cn" &&
               captured.at("MAILAI_IMAP_HOST") == "mail.bjtu.edu.cn" &&
               captured.at("MAILAI_IMAP_USER") == "student@bjtu.edu.cn" &&
               captured.at("MAILAI_IMAP_TLS") == "implicit" &&
               captured.at("MAILAI_IMAP_PORT") == "993" &&
               captured.at("MAILAI_DATA_DIR") == "/test/home/.local/share/mail-ai-assistant" &&
               !captured.contains("MAILAI_PASSWORD_ENV"),
        "fresh login uses known provider and secure transport defaults");
    for (const auto& [key, value] : captured)
        expect(!contains(value, secret), "configuration never contains the password");
    expect(!contains(result.out, secret) && !contains(result.err, secret),
        "login output never contains password");

    services.login_defaults = [] {
        return Result<Environment>{
            {{"HOME", "/test/home"},
                {"MAILAI_EMAIL", "user@example.test"},
                {"MAILAI_IMAP_HOST", "mail.example.test"},
                {"MAILAI_IMAP_USER", "other-user"},
                {"MAILAI_IMAP_TLS", "implicit"},
                {"MAILAI_IMAP_PORT", "999"}}
        };
    };
    lines = std::istringstream("\n\n\nstarttls\n\n~/chosen\n");
    result = invoke_with({"login"}, services, {}, input);
    expect(result.code == 0 && captured.at("MAILAI_IMAP_HOST") == "mail.example.test" &&
               captured.at("MAILAI_IMAP_USER") == "other-user" &&
               captured.at("MAILAI_IMAP_TLS") == "starttls" &&
               captured.at("MAILAI_IMAP_PORT") == "143" &&
               captured.at("MAILAI_DATA_DIR") == "/test/home/chosen",
        "remembered settings are accepted and changing TLS selects matching default port");
}

void test_login_cancellation_and_failure() {
    int calls{};
    int password_reads{};
    const std::string secret = "test-login-password";
    MailServices services;
    services.login_defaults = [] { return Result<Environment>{{{"HOME", "/test/home"}}}; };
    services.login = [&](const Environment&, std::string_view) -> Status {
        ++calls;
        return std::unexpected(
            Error{ErrorCode::auth_error, "Invalid password: " + secret, {}, {}, "imap.connect"});
    };
    std::istringstream lines("user@example.test\nmail.example.test\n\n\n\n\n");
    cli::LoginInput input{
        [&](std::string& line) { return static_cast<bool>(std::getline(lines, line)); },
        [&](std::string&) {
            ++password_reads;
            return false;
        }
    };
    expect_failure(invoke_with({"login"}, services), 1);
    expect_failure(invoke_with({"login"}, services, {}, input), 1);
    expect(calls == 0 && password_reads == 1, "cancelled password does not call login service");

    lines = std::istringstream("");
    expect_failure(invoke_with({"login"}, services, {}, input), 1);
    expect(calls == 0 && password_reads == 1, "cancelled input never asks for password");

    lines = std::istringstream("user@example.test\n\n");
    expect_failure(invoke_with({"login"}, services, {}, input), 1);
    expect(calls == 0 && password_reads == 1,
        "unknown provider cannot silently supply a guessed host");

    lines = std::istringstream("user@example.test\nimap://mail.example.test\n\n\n\n\n");
    const auto invalid_host = invoke_with({"login"}, services, {}, input);
    expect_failure(invalid_host, 1);
    expect(calls == 0 && password_reads == 1,
        "a server URL is rejected before prompting for a password");

    lines = std::istringstream("user@example.test\nmail.example.test\n\n\n65536\n\n");
    expect_failure(invoke_with({"login"}, services, {}, input), 1);
    expect(calls == 0 && password_reads == 1, "invalid port rejected before password prompt");

    lines = std::istringstream("user@example.test\nmail.example.test\n\n\n\n\n");
    input.read_password = [&](std::string& password) {
        password = secret;
        return true;
    };
    const auto result = invoke_with({"login"}, services, {}, input);
    expect_failure(result, 1);
    expect(calls == 1 && contains(result.err, "auth_error") && !contains(result.err, secret),
        "login failure preserves error classification but never exposes password");
}

void test_logout() {
    MailServices services;
    int calls{};
    services.logout = [&]() -> Status {
        ++calls;
        return {};
    };
    const auto result = invoke_with({"logout"}, services);
    expect_success(result);
    expect(calls == 1 && contains(result.out, "缓存"), "logout reports cached mail is retained");
    services.logout = []() -> Status {
        return std::unexpected(Error{ErrorCode::storage_error, "Logout failed", {}, {}});
    };
    expect_failure(invoke_with({"logout"}, services), 1);
}

}  // namespace

int main() {
    try {
        test_help();
        test_inbox_all();
        test_id_order_and_full_subject();
        test_find_command();
        test_account_filter();
        test_empty_inbox();
        test_show_by_stable_id();
        test_runtime_errors();
        test_bad_arguments();
        test_account_option_requires_value();
        test_validation_precedes_service_calls();
        test_cache_and_terminal_display();
        test_narrow_and_styled_table();
        test_sync_exit_codes();
        test_sync_history_options();
        test_login_and_logout_arguments();
        test_login_defaults_and_no_secret_output();
        test_login_cancellation_and_failure();
        test_logout();

        std::cout << "CLI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}

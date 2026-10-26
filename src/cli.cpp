#include "cli.h"

#include "mailai/inbox.h"

#include <charconv>
#include <ostream>
#include <string_view>
#include <system_error>
#include <variant>

namespace mailai::cli {

namespace {

void print_help(std::ostream& out) {
    out << "Usage:\n"
           "  mail_assistant inbox [--account <id|all>]\n"
           "  mail_assistant show <mail-id>\n"
           "  mail_assistant --help\n";
}

int argument_error(std::ostream& err, std::string_view message) {
    err << message << '\n';
    return 2;
}

int query_error(const Error& error, std::ostream& err) {
    err << error.message;
    if(error.account_id) {
        err << " [account=" << *error.account_id << ']';
    }
    if(error.mail_id) {
        err << " [mail=" << *error.mail_id << ']';
    }
    err << '\n';
    return 1;
}

} // namespace

int run_cli(
    const std::vector<std::string>& args,
    const std::vector<Account>& accounts,
    const std::vector<Mail>& mails,
    std::ostream& out,
    std::ostream& err
) {
    if(args.empty() || (args.size() == 1 && args[0] == "--help")) {
        print_help(out);
        return 0;
    }

    const auto& command = args[0];

    if(command != "inbox" && command != "show") {
        return argument_error(err, "未知命令，请使用 --help 查看用法");
    }

    if(args.size() == 2 && args[1] == "--help") {
        print_help(out);
        return 0;
    }

    if(command == "inbox") {
        InboxQuery query{};

        if(args.size() != 1) {
            if(args.size() != 3 || args[1] != "--account") {
                return argument_error(
                    err, "用法: inbox [--account <id|all>]"
                );
            }
            const auto& account_id = args[2];
            if(account_id.empty() || account_id.starts_with("--")) {
                return argument_error(err, "--account 后需要账号 ID 或 all");
            }
            if(account_id != "all") {
                query.account_id = account_id;
            }
        }

        const auto result = list_inbox(accounts, mails, query);

        if(const auto* error = std::get_if<Error>(&result)) {
            return query_error(*error, err);
        }

        const auto& items = std::get<std::vector<InboxItem>>(result);

        out << "数据来源：示例\n"
               "ID\t账号\t发件人\t主题\n";

        for(const auto& item : items) {
            out << item.id << '\t'
                << item.account_name << '\t'
                << item.sender << '\t'
                << item.subject << '\n';
        }

        out << "共 " << items.size() << " 封\n";
        return 0;
    }

    // 剩下的命令是 show
    if(args.size() != 2) {
        return argument_error(err, "用法: show <mail-id>");
    }

    MailId id{};
    const char* begin = args[1].data();
    const char* end = begin + args[1].size();

    const auto [ptr, ec] = std::from_chars(begin, end, id, 10);

    if(ec != std::errc{} || ptr != end || id == 0) {
        return argument_error(err, "邮件 ID 必须是范围内的十进制整数");
    }

    const auto result = find_mail(mails, id);

    if(const auto* error = std::get_if<Error>(&result)) {
        return query_error(*error, err);
    }

    const auto& mail = std::get<Mail>(result);

    out << "数据来源：示例\n"
        << "ID: " << mail.id << "\n"
        << "账号: " << mail.account_id << "\n"
        << "发件人: " << mail.sender << "\n"
        << "主题: " << mail.subject << "\n"
        << "正文:\n" << mail.body << "\n";

    return 0;
}

} // namespace mailai::cli

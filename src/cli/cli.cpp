#include "cli.h"

#include <algorithm>
#include <charconv>
#include <expected>
#include <limits>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cli_output.h"
#include "mailai/error_message.h"

namespace mailai::cli {
namespace {

enum class CommandKind { help, inbox, show, sync, login, logout };

struct Command {
    CommandKind kind{CommandKind::help};
    DataSource source{DataSource::cache};
    InboxQuery query;
    MailId id{};
    std::size_t history_limit{};
    bool advanced_login{};
};

using ParseResult = std::expected<Command, std::string_view>;

ParseResult parse_inbox(std::span<const std::string> args, Command command) {
    if (args.size() == 1)
        return command;
    if (args.size() != 3 || args[1] != "--account")
        return std::unexpected(error_message::cli::inbox_usage);
    const auto& account = args[2];
    if (account.empty() || account.starts_with("--"))
        return std::unexpected(error_message::cli::account_required);
    if (account != "all")
        command.query.account_id = account;
    return command;
}

ParseResult parse_show(std::span<const std::string> args, Command command) {
    if (args.size() != 2)
        return std::unexpected(error_message::cli::show_usage);
    const auto* begin = args[1].data();
    const auto* end = begin + args[1].size();
    const auto [ptr, ec] = std::from_chars(begin, end, command.id, 10);
    if (ec != std::errc{} || ptr != end || command.id == 0)
        return std::unexpected(error_message::cli::invalid_id);
    return command;
}

ParseResult parse_sync(std::span<const std::string> args, Command command) {
    if (command.source == DataSource::demo)
        return std::unexpected(error_message::cli::demo_sync_forbidden);
    if (args.size() == 1)
        return command;
    if (args.size() == 2 && args[1] == "--all") {
        command.history_limit = std::numeric_limits<std::size_t>::max();
        return command;
    }
    if (args.size() != 3 || args[1] != "--history")
        return std::unexpected(error_message::cli::sync_usage);
    const auto* begin = args[2].data();
    const auto* end = begin + args[2].size();
    const auto [ptr, ec] = std::from_chars(begin, end, command.history_limit);
    if (ec != std::errc{} || ptr != end || command.history_limit == 0)
        return std::unexpected(error_message::cli::invalid_history_count);
    return command;
}

ParseResult parse_command(std::span<const std::string> args) {
    Command command;
    if (args.empty())
        return command;
    if (args.front() == "--demo") {
        command.source = DataSource::demo;
        args = args.subspan(1);
        if (args.empty())
            return std::unexpected(error_message::cli::demo_command_required);
    }
    if (args.size() == 1 && args.front() == "--help")
        return command;

    if (args.front() == "inbox" || args.front() == "find")
        command.kind = CommandKind::inbox;
    else if (args.front() == "show")
        command.kind = CommandKind::show;
    else if (args.front() == "sync")
        command.kind = CommandKind::sync;
    else if (args.front() == "login")
        command.kind = CommandKind::login;
    else if (args.front() == "logout")
        command.kind = CommandKind::logout;
    else
        return std::unexpected(error_message::cli::unknown_command);

    if (command.source == DataSource::demo &&
        (command.kind == CommandKind::login || command.kind == CommandKind::logout))
        return std::unexpected(error_message::login_cli::demo_forbidden);

    if (args.size() == 2 && args[1] == "--help") {
        command.kind = CommandKind::help;
        return command;
    }

    switch (command.kind) {
        case CommandKind::inbox:
            if (args.front() == "find") {
                if (args.size() != 2 || args[1].empty())
                    return std::unexpected(error_message::cli::find_usage);
                command.query.keyword = args[1];
                return command;
            }
            return parse_inbox(args, command);
        case CommandKind::show:
            return parse_show(args, command);
        case CommandKind::sync:
            return parse_sync(args, command);
        case CommandKind::login:
            if (args.size() == 2 && args[1] == "--advanced") {
                command.advanced_login = true;
                return command;
            }
            if (args.size() != 1)
                return std::unexpected(error_message::login_cli::login_usage);
            return command;
        case CommandKind::logout:
            if (args.size() != 1)
                return std::unexpected(error_message::login_cli::logout_usage);
            return command;
        case CommandKind::help:
            return command;
    }
    return std::unexpected(error_message::cli::invalid_command);
}

int execute_inbox(const Command& command,
    const MailServices& services,
    std::ostream& out,
    std::ostream& err,
    const TerminalOptions& terminal) {
    auto result = services.list(command.source, command.query);
    if (!result) {
        print_error(err, result.error());
        return 1;
    }
    std::ranges::sort(*result, {}, &InboxItem::id);
    print_inbox(out, command.source, *result, terminal);
    return 0;
}

int execute_show(const Command& command,
    const MailServices& services,
    std::ostream& out,
    std::ostream& err,
    const TerminalOptions& terminal) {
    auto result = services.find(command.source, command.id);
    if (!result) {
        print_error(err, result.error());
        return 1;
    }
    print_mail(out, command.source, *result, terminal);
    return 0;
}

int execute_sync(
    const Command& command, const MailServices& services, std::ostream& out, std::ostream& err) {
    auto result = services.sync(command.history_limit);
    if (!result) {
        print_error(err, result.error());
        return 1;
    }
    print_sync_summary(out, err, *result);
    if (result->failed == 0)
        return 0;
    return result->success == 0 ? 1 : 3;
}

}  // namespace

int run_cli(const std::vector<std::string>& args,
    const MailServices& services,
    std::ostream& out,
    std::ostream& err,
    const TerminalOptions& terminal,
    const LoginInput& login_input) {
    // 参数全部校验通过后才允许调用业务服务。
    auto command = parse_command(args);
    if (!command) {
        err << command.error() << '\n';
        return 2;
    }

    switch (command->kind) {
        case CommandKind::help:
            print_help(out);
            return 0;
        case CommandKind::inbox:
            return execute_inbox(*command, services, out, err, terminal);
        case CommandKind::show:
            return execute_show(*command, services, out, err, terminal);
        case CommandKind::sync:
            return execute_sync(*command, services, out, err);
        case CommandKind::login:
            return execute_login(services, out, err, login_input, command->advanced_login);
        case CommandKind::logout:
            return execute_logout(services, out, err);
    }
    return 2;
}

}  // namespace mailai::cli

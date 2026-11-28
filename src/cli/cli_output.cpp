#include "cli_output.h"

#include <array>
#include <cstdint>
#include <ctime>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace mailai::cli {
namespace {

std::string display_time(std::int64_t seconds, bool compact = false) {
    if (std::in_range<std::time_t>(seconds)) {
        const auto timestamp = static_cast<std::time_t>(seconds);
        std::tm local{};
        char buffer[64]{};
        if (::localtime_r(&timestamp, &local) &&
            std::strftime(buffer,
                sizeof(buffer),
                compact ? "%Y-%m-%d %H:%M" : "%Y-%m-%d %H:%M:%S %z",
                &local) != 0)
            return buffer;
    }
    return "UTC秒 " + std::to_string(seconds);
}

void print_source(std::ostream& out, DataSource source) {
    out << "数据来源：" << (source == DataSource::demo ? "示例" : "缓存") << '\n';
}

std::string display_state(ParseState state, bool historical) {
    std::string result = state == ParseState::failed ? "解析失败" : "已解析";
    if (historical)
        result += " / 历史邮件";
    return result;
}

void print_field(std::ostream& out, std::string_view name, std::string_view value) {
    out << name << ": ";
    print_text(out, value);
    out << '\n';
}

void print_body_table(std::ostream& out, const BodyTable& table, const TerminalOptions& terminal) {
    if (table.rows.empty() || table.rows.front().empty())
        return;
    std::vector<TableColumn> columns;
    for (const auto& title : table.rows.front()) columns.push_back({title, 0, true, true, true});
    if (table.rows.size() == 1) {
        for (auto& column : columns) column.title = {};
        print_table(out, columns, table.rows, terminal);
    } else {
        print_table(out, columns, std::span{table.rows}.subspan(1), terminal);
    }
}

}  // namespace

void print_help(std::ostream& out) {
    out << "Usage:\n"
           "  mail_assistant login\n"
           "  mail_assistant logout\n"
           "  mail_assistant sync [--history <count> | --all]\n"
           "  mail_assistant [--demo] inbox [--account <id|all>]\n"
           "  mail_assistant [--demo] find <keyword>\n"
           "  mail_assistant [--demo] show <mail-id>\n"
           "  mail_assistant --help\n"
           "login 交互配置账号；logout 清除登录凭据并保留邮件缓存。\n"
           "默认读取本地缓存；--demo 仅查看虚构邮件。\n";
}

void print_error(std::ostream& err, const Error& error) {
    print_text(err, error.message);
    err << " [code=" << error_code_name(error.code) << ']';
    if (error.account_id) {
        err << " [account=";
        print_text(err, *error.account_id);
        err << ']';
    }
    if (error.mail_id)
        err << " [mail=" << *error.mail_id << ']';
    if (!error.stage.empty()) {
        err << " [stage=";
        print_text(err, error.stage);
        err << ']';
    }
    err << '\n';
}

void print_inbox(std::ostream& out,
    DataSource source,
    const std::vector<InboxItem>& items,
    const TerminalOptions& terminal) {
    print_source(out, source);
    if (items.empty()) {
        out << "暂无邮件\n共 0 封\n";
        return;
    }
    constexpr std::array columns{
        TableColumn{"ID", 0, false},
        TableColumn{"账号", 16},
        TableColumn{"发件人", 36},
        TableColumn{"主题", 0, true, true},
        TableColumn{"接收时间", 0, false},
        TableColumn{"状态", 0, false}
    };
    std::vector<TableRow> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        rows.push_back(
            {std::to_string(item.id),
                item.account_name,
                item.sender,
                item.subject,
                display_time(item.received_at_utc, true),
                display_state(item.parse_state, item.historical)});
    }
    out << '\n';
    print_table(out, columns, rows, terminal);
    out << '\n';
    out << "共 " << items.size() << " 封\n";
}

void print_mail(
    std::ostream& out, DataSource source, const Mail& mail, const TerminalOptions& terminal) {
    print_source(out, source);
    out << "ID: " << mail.id << '\n';
    print_field(out, "账号", mail.account_id);
    print_field(out, "发件人", mail.sender);
    print_field(out, "主题", mail.subject);
    print_field(out, "接收时间", display_time(mail.received_at_utc));
    print_field(out, "状态", display_state(mail.parse_state, mail.historical));
    if (mail.parse_state == ParseState::failed)
        print_field(out, "解析错误", mail.parse_error);
    out << "附件: " << mail.attachments.size() << " 个\n";
    for (const auto& attachment : mail.attachments) {
        out << "  ";
        print_text(out, attachment.name);
        out << " (";
        print_text(out, attachment.media_type);
        out << ", " << attachment.size_bytes << " 字节)\n";
    }
    out << "正文:\n";
    if (mail.body_blocks.empty()) {
        print_text(out, mail.body, true);
        out << '\n';
        return;
    }
    for (const auto& block : mail.body_blocks) {
        if (const auto* text = std::get_if<std::string>(&block))
            print_text(out, *text, true);
        else
            print_body_table(out, std::get<BodyTable>(block), terminal);
        out << '\n';
    }
}

void print_sync_summary(std::ostream& out, std::ostream& err, const SyncSummary& summary) {
    print_source(out, DataSource::cache);
    if (summary.reindexed)
        out << "UIDVALIDITY 已变化，重新索引收件箱。\n";
    out << "同步结果：新增 " << summary.discovered << " 封，成功 " << summary.success
        << " 封，失败 " << summary.failed << " 封\n";
    for (const auto& failure : summary.failures) {
        err << "UID " << failure.key.uid << " (UIDVALIDITY " << failure.key.uid_validity << "): ";
        print_error(err, failure.error);
    }
}

}  // namespace mailai::cli

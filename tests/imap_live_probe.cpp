#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>

#include "mailai/config.h"
#include "mailai/error_message.h"
#include "mailai/imap.h"
#include "mailai/mime.h"

namespace {

using namespace mailai;
namespace errors = error_message::live_probe;

constexpr std::size_t probe_limit = 5;

void print_failure(const Error& error) {
    // 只报告错误类型、阶段及模块生成的诊断，不输出账号和邮件信息。
    std::cerr << errors::failed << ": " << error_code_name(error.code);
    if (!error.stage.empty())
        std::cerr << " [" << error.stage << ']';
    std::cerr << ": " << error.message << '\n';
}

int probe(const RuntimeConfig& config) {
    auto snapshot = read_mailbox_snapshot(config);
    if (!snapshot) {
        print_failure(snapshot.error());
        return 1;
    }

    const auto count = std::min(snapshot->uids.size(), probe_limit);
    std::size_t downloaded = 0;
    std::size_t parsed = 0;
    std::size_t attachments = 0;
    // 快照已按 UID 排序并去重，只试收最近五封，不打开数据库。
    for (auto it = snapshot->uids.end() - count; it != snapshot->uids.end(); ++it) {
        const RemoteKey key{config.account.id, "INBOX", snapshot->uid_validity, *it};
        auto mail = fetch_mail(config, key);
        if (!mail) {
            print_failure(mail.error());
            continue;
        }
        ++downloaded;

        auto result = parse_message(mail->raw);
        if (!result) {
            print_failure(result.error());
            continue;
        }
        ++parsed;
        attachments += result->attachments.size();
    }
    const auto failed = count - parsed;
    std::cout << "试收结果：尝试 " << count << " 封，下载成功 " << downloaded << " 封，解析成功 "
              << parsed << " 封，失败 " << failed << " 封，附件 " << attachments << " 个\n";
    if (failed == 0)
        return 0;
    return parsed == 0 ? 1 : 3;
}

}  // namespace

int main() {
    const char* enabled = std::getenv("MAILAI_LIVE_TESTS");
    if (!enabled || std::string_view{enabled} != "1") {
        std::cerr << errors::enable_required << '\n';
        return 2;
    }
    try {
        auto config = parse_sync_config(read_runtime_environment());
        if (!config) {
            print_failure(config.error());
            return 1;
        }
        return probe(*config);
    } catch (const std::exception&) {
        std::cerr << errors::failed << '\n';
        return 1;
    }
}

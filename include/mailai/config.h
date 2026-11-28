#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "mailai/model.h"

namespace mailai {

using Environment = std::map<std::string, std::string>;

enum class TlsMode { implicit, starttls };

struct ImapSettings {
    std::string host;
    std::string username;
    std::string credential_env;
    std::uint16_t port{993};
    TlsMode tls_mode{TlsMode::implicit};
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds total_timeout{30000};
    std::optional<std::filesystem::path> ca_file;
};

struct RuntimeConfig {
    Account account;
    ImapSettings imap;
    std::filesystem::path data_dir;
    // 运行时凭据读取器不参与配置或数据库持久化；为空时沿用环境变量。
    std::function<Result<std::string>()> credential_provider{};
};

// 环境变量引用只允许变量名，不允许将赋值表达式写入配置或数据库。
constexpr bool valid_credential_reference(std::string_view name) noexcept {
    return !name.empty() && name.find('=') == std::string_view::npos &&
           name.find('\0') == std::string_view::npos;
}

// 仅读取配置项与凭据引用，不读取密码值。
Environment read_runtime_environment();

Result<std::filesystem::path> resolve_data_dir(const Environment& env);

Result<RuntimeConfig> parse_sync_config(const Environment& env);

}  // namespace mailai

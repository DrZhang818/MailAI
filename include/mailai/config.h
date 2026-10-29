#pragma once

#include "mailai/model.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

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
};

Result<std::filesystem::path> resolve_data_dir(const Environment& env);

Result<RuntimeConfig> parse_sync_config(const Environment& env);

} // namespace mailai
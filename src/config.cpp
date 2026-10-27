#include "mailai/config.h"

#include <charconv>
#include <system_error>

namespace mailai {

namespace {

std::string config_value(
    const Environment& env,
    const std::string& name,
    const std::string& fallback = {}
) {
    const auto it = env.find(name);
    if(it != env.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

Error make_config_error(const std::string& message) {
    return Error{
        ErrorCode::config_error,
        message,
        std::nullopt,
        std::nullopt,
    };
}

} // namespace

Result<std::filesystem::path> resolve_data_dir(const Environment& env) {
    if(const auto it = env.find("MAILAI_DATA_DIR"); it != env.end() && !it->second.empty()) {
        return std::filesystem::path{it->second};
    }
    if(const auto it = env.find("XDG_DATA_HOME"); it != env.end() && !it->second.empty()) {
        const std::filesystem::path xdg_dir{it->second};
        if(xdg_dir.is_absolute()) {
            return xdg_dir / "mail-ai-assistant";
        }
    }
    if(const auto it = env.find("HOME"); it != env.end() && !it->second.empty()) {
        return std::filesystem::path(it->second) / ".local" / "share" / "mail-ai-assistant";
    }
    return std::unexpected(Error{
        ErrorCode::config_error,
        "Cannot determine data directory",
        std::nullopt,
        std::nullopt
    });
}

Result<RuntimeConfig> parse_sync_config(const Environment& env) {
    for(const char* name : {
        "MAILAI_EMAIL",
        "MAILAI_IMAP_HOST",
        "MAILAI_IMAP_USER"
    }) {
        if(config_value(env, name).empty()) {
            return std::unexpected(make_config_error(
                "Missing required setting: " + std::string(name)
            ));
        }
    }

    const auto directory = resolve_data_dir(env);

    if(!directory) {
        return std::unexpected(directory.error());
    }

    RuntimeConfig config;

    config.data_dir = *directory;
    
    config.account.id = 
        config_value(env, "MAILAI_ACCOUNT_ID", "school");
    
    config.account.display_name = 
        config_value(env, "MAILAI_ACCOUNT_NAME", "学校邮箱");
    
    config.account.address = 
        config_value(env, "MAILAI_EMAIL");

    config.imap.host =
        config_value(env, "MAILAI_IMAP_HOST");

    config.imap.username =
        config_value(env, "MAILAI_IMAP_USER");

    config.imap.credential_env = 
        config_value(env, "MAILAI_PASSWORD_ENV", "MAILAI_IMAP_PASSWORD");

    auto tls = 
        config_value(env, "MAILAI_IMAP_TLS", "implicit");
    
    if(tls == "starttls") {
        config.imap.tls_mode = TlsMode::starttls;
        config.imap.port = 143;
    } else if(tls != "implicit") {
        return std::unexpected(make_config_error(
            "MAILAI_IMAP_TLS must be implicit or starttls"
        ));
    }

    const auto port_text = 
        config_value(env, "MAILAI_IMAP_PORT");

    if(!port_text.empty()) {
        std::uint32_t port{};

        const auto* begin = port_text.data();
        const auto* end = port_text.data() + port_text.size();

        const auto [ptr, ec] = std::from_chars(begin, end, port);
        if(ec != std::errc{} || ptr != end || port == 0 || port > 65535) {
            return std::unexpected(make_config_error(
                "MAILAI_IMAP_PORT must be a decimal integer from 1 to 65535"
            ));
        }

        config.imap.port = static_cast<std::uint16_t>(port);
    }

    return config;
}

} // namespace mailai

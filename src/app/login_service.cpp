#include "login_service.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "mailai/data_dir_lock.h"
#include "mailai/error_message.h"
#include "mailai/login_profile.h"
#include "mailai/store.h"

namespace mailai::login_detail {
namespace {

namespace errors = error_message::login;

constexpr std::array identity_settings{"MAILAI_EMAIL", "MAILAI_IMAP_HOST", "MAILAI_IMAP_USER"};

constexpr std::array nonsecret_settings{
    "MAILAI_EMAIL",
    "MAILAI_IMAP_HOST",
    "MAILAI_IMAP_USER",
    "MAILAI_ACCOUNT_ID",
    "MAILAI_ACCOUNT_NAME",
    "MAILAI_IMAP_TLS",
    "MAILAI_IMAP_PORT",
    "MAILAI_IMAP_CA_FILE",
    "MAILAI_DATA_DIR"
};

std::unexpected<Error> login_error(ErrorCode code, std::string_view message) {
    return std::unexpected(Error{code, std::string{message}, {}, {}, "login"});
}

bool set_value(const Environment& env, const char* name) {
    const auto it = env.find(name);
    return it != env.end() && !it->second.empty();
}

bool explicit_mailbox(const Environment& env) {
    for (const auto* name : identity_settings)
        if (env.contains(name))
            return true;
    return false;
}

bool has_profile_location(const Environment& env) {
    if (const auto it = env.find("XDG_CONFIG_HOME");
        it != env.end() && std::filesystem::path{it->second}.is_absolute())
        return true;
    return set_value(env, "HOME");
}

void copy_setting(Environment& target, const Environment& source, const char* name) {
    if (const auto it = source.find(name); it != source.end() && !it->second.empty())
        target[name] = it->second;
}

// 钥匙串分键同时绑定配置位置和服务器身份，避免将保存的密码用于不同服务器。
std::string credential_key(const std::filesystem::path& directory, const RuntimeConfig& config) {
    std::string key;
    const auto append = [&](std::string_view part) {
        key += std::to_string(part.size());
        key += ':';
        key += part;
    };
    append(directory.lexically_normal().string());
    append(config.account.address);
    append(config.imap.host);
    append(config.imap.username);
    append(std::to_string(config.imap.port));
    append(config.imap.tls_mode == TlsMode::implicit ? "implicit" : "starttls");
    return key;
}

Environment profile_from(const RuntimeConfig& config) {
    Environment profile{
        {"MAILAI_EMAIL", config.account.address},
        {"MAILAI_IMAP_HOST", config.imap.host},
        {"MAILAI_IMAP_USER", config.imap.username},
        {"MAILAI_ACCOUNT_ID", config.account.id},
        {"MAILAI_ACCOUNT_NAME", config.account.display_name},
        {"MAILAI_IMAP_TLS", config.imap.tls_mode == TlsMode::implicit ? "implicit" : "starttls"},
        {"MAILAI_IMAP_PORT", std::to_string(config.imap.port)},
        {"MAILAI_DATA_DIR", config.data_dir.string()}
    };
    if (config.imap.ca_file)
        profile.emplace("MAILAI_IMAP_CA_FILE", config.imap.ca_file->string());
    return profile;
}

bool valid_profile_values(const Environment& profile) {
    for (const auto& [name, value] : profile) {
        if (value.empty() || value.find_first_of("\r\n") != std::string::npos ||
            value.find('\0') != std::string::npos)
            return false;
    }
    return true;
}

bool valid_host(std::string_view host) {
    return !host.empty() && host.find_first_of("/\\@?# \t\r\n") == std::string_view::npos &&
           host.find('\0') == std::string_view::npos;
}

Result<std::filesystem::path> canonical_profile_directory(const Environment& env) {
    auto directory = resolve_login_profile_dir(env);
    if (!directory)
        return std::unexpected(directory.error());
    std::error_code error;
    auto canonical = std::filesystem::canonical(*directory, error);
    if (error)
        return login_error(ErrorCode::config_error, errors::canonical_profile_failed);
    return canonical;
}

Status validate_cache_destination(const RuntimeConfig& config) {
    std::error_code error;
    const auto directory = std::filesystem::status(config.data_dir, error);
    if (error == std::errc::no_such_file_or_directory)
        return {};
    if (error || !std::filesystem::is_directory(directory))
        return login_error(ErrorCode::config_error, errors::invalid_cache_directory);
    const auto path = config.data_dir / "cache.sqlite3";
    const auto cache = std::filesystem::status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return {};
    if (error || !std::filesystem::is_regular_file(cache))
        return login_error(ErrorCode::config_error, errors::invalid_cache_directory);
    auto store = SqliteStore::open(path, OpenMode::read_only);
    if (!store)
        return std::unexpected(store.error());
    return store->check_account_identity(config.account, config.imap);
}

Result<std::optional<Environment>> remembered_profile(const Environment& env) {
    if (!has_profile_location(env))
        return std::optional<Environment>{};
    return load_login_profile(env);
}

Result<std::string> previous_key(
    const std::filesystem::path& directory, const std::optional<Environment>& profile) {
    if (!profile)
        return std::string{};
    auto parsed = parse_sync_config(*profile);
    if (!parsed)
        return std::unexpected(parsed.error());
    return credential_key(directory, *parsed);
}

}  // namespace

Result<Environment> login_defaults(const Environment& env) {
    auto saved = remembered_profile(env);
    if (!saved)
        return std::unexpected(saved.error());

    Environment defaults = saved->value_or(Environment{});
    // 显式身份发生变化时，不把旧服务器或旧用户名作为新邮箱的默认值。
    bool changed = false;
    for (const auto* name : identity_settings) {
        if (set_value(env, name) && set_value(defaults, name) && env.at(name) != defaults.at(name))
            changed = true;
    }
    if (changed) {
        for (const auto* name : nonsecret_settings)
            if (std::string_view{name} != "MAILAI_DATA_DIR")
                defaults.erase(name);
    }
    for (const auto* name : nonsecret_settings) copy_setting(defaults, env, name);
    for (const auto* name : {"HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME"})
        copy_setting(defaults, env, name);

    auto data_dir = resolve_data_dir(defaults);
    if (!data_dir)
        return std::unexpected(data_dir.error());
    defaults["MAILAI_DATA_DIR"] = data_dir->string();
    return defaults;
}

Result<std::filesystem::path> resolve_cache_directory(const Environment& env) {
    if (set_value(env, "MAILAI_DATA_DIR") || explicit_mailbox(env))
        return resolve_data_dir(env);
    auto saved = remembered_profile(env);
    if (!saved)
        return std::unexpected(saved.error());
    if (*saved)
        return resolve_data_dir(**saved);
    return resolve_data_dir(env);
}

Result<RuntimeConfig> resolve_sync_settings(
    const Environment& env, const CredentialStore& credentials) {
    // 显式配置完整优先，不把新邮箱设置与旧钥匙串密码混合。
    if (explicit_mailbox(env))
        return parse_sync_config(env);
    auto saved = remembered_profile(env);
    if (!saved)
        return std::unexpected(saved.error());
    if (!*saved)
        return login_error(ErrorCode::config_error, errors::login_required);
    if (!credentials.read)
        return login_error(ErrorCode::config_error, errors::invalid_service);

    auto directory = canonical_profile_directory(env);
    if (!directory)
        return std::unexpected(directory.error());
    // 登录或退出也持有此锁，避免读取旧配置后发现其凭据已被替换。
    auto lock = DataDirLock::acquire(*directory);
    if (!lock)
        return std::unexpected(lock.error());
    saved = load_login_profile(env);
    if (!saved)
        return std::unexpected(saved.error());
    if (!*saved)
        return login_error(ErrorCode::config_error, errors::login_required);

    auto config = parse_sync_config(**saved);
    if (!config)
        return std::unexpected(config.error());
    if (set_value(env, "MAILAI_DATA_DIR"))
        config->data_dir = std::filesystem::path{env.at("MAILAI_DATA_DIR")};
    auto password = credentials.read(credential_key(*directory, *config));
    if (!password)
        return std::unexpected(password.error());
    if (!*password || (**password).empty() || (**password).find('\0') != std::string::npos)
        return login_error(ErrorCode::auth_error, error_message::keychain::missing_password);
    config->credential_provider = [password = std::move(**password)] { return password; };
    return config;
}

Status remember_login(const Environment& env,
    const Environment& input,
    std::string_view password,
    const CredentialStore& credentials,
    const VerifyLogin& verify) {
    if (!credentials.read || !credentials.write || !credentials.erase || !verify)
        return login_error(ErrorCode::config_error, errors::invalid_service);
    if (password.empty() || password.find('\0') != std::string_view::npos)
        return login_error(ErrorCode::invalid_argument, errors::invalid_login);

    Environment settings;
    for (const auto* name : nonsecret_settings) copy_setting(settings, input, name);
    for (const auto* name : {"HOME", "XDG_DATA_HOME"}) copy_setting(settings, env, name);
    auto config = parse_sync_config(settings);
    if (!config)
        return std::unexpected(config.error());
    if (!valid_host(config->imap.host))
        return login_error(ErrorCode::invalid_argument, errors::invalid_login);

    std::error_code error;
    config->data_dir = std::filesystem::absolute(config->data_dir, error).lexically_normal();
    if (error)
        return login_error(ErrorCode::config_error, errors::absolute_data_dir_failed);
    if (config->imap.ca_file) {
        *config->imap.ca_file =
            std::filesystem::absolute(*config->imap.ca_file, error).lexically_normal();
        if (error)
            return login_error(ErrorCode::config_error, errors::absolute_ca_failed);
    }
    if (auto compatible = validate_cache_destination(*config); !compatible)
        return compatible;
    const auto profile = profile_from(*config);
    if (!valid_profile_values(profile))
        return login_error(ErrorCode::invalid_argument, errors::invalid_login);
    auto directory = resolve_login_profile_dir(env);
    if (!directory)
        return std::unexpected(directory.error());

    // 登录仅验证认证与收件箱访问，不下载或修改邮件。
    config->credential_provider = [password] { return std::string{password}; };
    if (auto verified = verify(*config); !verified)
        return std::unexpected(verified.error());

    if (auto ready = ensure_login_profile_dir(env); !ready)
        return ready;
    directory = canonical_profile_directory(env);
    if (!directory)
        return std::unexpected(directory.error());
    auto lock = DataDirLock::acquire(*directory);
    if (!lock)
        return std::unexpected(lock.error());
    auto old = load_login_profile(env);
    if (!old)
        return std::unexpected(old.error());
    auto old_key = previous_key(*directory, *old);
    if (!old_key)
        return std::unexpected(old_key.error());
    const auto new_key = credential_key(*directory, *config);
    std::optional<std::string> old_password;
    if (!old_key->empty()) {
        auto loaded = credentials.read(*old_key);
        if (!loaded)
            return std::unexpected(loaded.error());
        old_password = std::move(*loaded);
    }
    if (auto stored = credentials.write(new_key, password); !stored)
        return stored;
    const bool changed_key = *old_key != new_key;
    if (changed_key && old_password) {
        if (auto removed = credentials.erase(*old_key); !removed) {
            if (auto rolled_back = credentials.erase(new_key); !rolled_back)
                return login_error(ErrorCode::storage_error, errors::rollback_failed);
            return removed;
        }
    }
    if (auto saved = save_login_profile(env, profile); !saved) {
        // 配置原子替换失败时恢复原凭据，并清除尚未提交的新身份。
        bool restored = true;
        if (changed_key || !old_password)
            restored = credentials.erase(new_key).has_value();
        if (old_password)
            restored = credentials.write(*old_key, *old_password).has_value() && restored;
        if (!restored)
            return login_error(ErrorCode::storage_error, errors::rollback_failed);
        return saved;
    }
    return {};
}

Status forget_login(const Environment& env, const CredentialStore& credentials) {
    auto saved = remembered_profile(env);
    if (!saved)
        return std::unexpected(saved.error());
    if (!*saved)
        return {};
    if (!credentials.erase)
        return login_error(ErrorCode::config_error, errors::invalid_service);
    auto directory = canonical_profile_directory(env);
    if (!directory)
        return std::unexpected(directory.error());
    auto lock = DataDirLock::acquire(*directory);
    if (!lock)
        return std::unexpected(lock.error());
    saved = load_login_profile(env);
    if (!saved)
        return std::unexpected(saved.error());
    if (!*saved)
        return {};
    auto key = previous_key(*directory, *saved);
    if (!key)
        return std::unexpected(key.error());
    return credentials.erase(*key);
}

}  // namespace mailai::login_detail

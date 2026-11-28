#include "cli_login.h"

#if defined(__APPLE__)
#include <readpassphrase.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

#include "cli_output.h"
#include "mailai/error_message.h"

namespace mailai::cli {
namespace {

void clear_bytes(char* buffer, std::size_t size) {
    volatile char* byte = buffer;
    for (std::size_t i = 0; i < size; ++i) byte[i] = 0;
}

struct PasswordBuffer {
    std::array<char, 4096> value{};

    ~PasswordBuffer() {
        clear_bytes(value.data(), value.size());
    }
};

struct Password {
    std::string value;

    ~Password() {
        clear_bytes(value.data(), value.size());
    }
};

std::string setting(const Environment& env, const std::string& key) {
    const auto it = env.find(key);
    return it == env.end() ? std::string{} : it->second;
}

bool has_whitespace_or_control(std::string_view value) {
    return std::ranges::any_of(value, [](unsigned char c) { return c <= 0x20 || c == 0x7f; });
}

bool valid_email(std::string_view email) {
    const auto at = email.find('@');
    return at != std::string_view::npos && at > 0 && at + 1 < email.size() &&
           email.find('@', at + 1) == std::string_view::npos && !has_whitespace_or_control(email);
}

std::string provider_host(std::string_view email) {
    std::string domain{email.substr(email.find('@') + 1)};
    std::ranges::transform(domain, domain.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
    });
    return domain == "bjtu.edu.cn" ? "mail.bjtu.edu.cn" : "";
}

bool prompt(std::ostream& err,
    const LoginInput& input,
    std::string_view label,
    std::string_view fallback,
    std::string& value) {
    err << label;
    if (!fallback.empty()) {
        err << " [";
        print_text(err, fallback);
        err << ']';
    }
    err << ": " << std::flush;
    std::string line;
    if (!input.read_line(line)) {
        err << error_message::login_cli::input_cancelled << '\n';
        return false;
    }
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        value.assign(fallback);
    else
        value = line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1);
    return true;
}

std::optional<Environment> ask_configuration(
    const Environment& defaults, const LoginInput& input, std::ostream& err) {
    Environment config;
    const auto saved_email = setting(defaults, "MAILAI_EMAIL");
    auto& email = config["MAILAI_EMAIL"];
    if (!prompt(err, input, "邮箱地址", saved_email, email))
        return std::nullopt;
    if (!valid_email(email)) {
        err << error_message::login_cli::invalid_email << '\n';
        return std::nullopt;
    }

    const bool same_account = saved_email.empty() || saved_email == email;
    const auto saved_host = setting(defaults, "MAILAI_IMAP_HOST");
    const auto default_host =
        same_account && !saved_host.empty() ? saved_host : provider_host(email);
    auto& host = config["MAILAI_IMAP_HOST"];
    if (!prompt(err, input, "IMAP 服务器", default_host, host))
        return std::nullopt;
    if (host.empty() || has_whitespace_or_control(host) ||
        host.find_first_of("/\\@?#") != std::string::npos) {
        err << error_message::login_cli::invalid_host << '\n';
        return std::nullopt;
    }

    const auto saved_user = setting(defaults, "MAILAI_IMAP_USER");
    auto& username = config["MAILAI_IMAP_USER"];
    if (!prompt(err,
            input,
            "IMAP 用户名",
            same_account && !saved_user.empty() ? saved_user : email,
            username))
        return std::nullopt;

    const bool same_host = same_account && host == saved_host;
    const auto saved_tls = setting(defaults, "MAILAI_IMAP_TLS");
    const std::string default_tls = same_host && !saved_tls.empty() ? saved_tls : "implicit";
    auto& tls = config["MAILAI_IMAP_TLS"];
    if (!prompt(err, input, "TLS 模式 (implicit/starttls)", default_tls, tls))
        return std::nullopt;
    if (tls != "implicit" && tls != "starttls") {
        err << error_message::config::invalid_tls << '\n';
        return std::nullopt;
    }

    const auto saved_port = setting(defaults, "MAILAI_IMAP_PORT");
    const std::string default_port = same_host && tls == default_tls && !saved_port.empty()
                                         ? saved_port
                                         : (tls == "implicit" ? "993" : "143");
    if (!prompt(err, input, "IMAP 端口", default_port, config["MAILAI_IMAP_PORT"]))
        return std::nullopt;

    const auto default_path = resolve_data_dir(defaults);
    const auto path_text = default_path ? default_path->string() : std::string{};
    auto& path = config["MAILAI_DATA_DIR"];
    if (!prompt(err, input, "邮件数据目录", path_text, path))
        return std::nullopt;
    if (path == "~" || path.starts_with("~/")) {
        const auto home = setting(defaults, "HOME");
        if (home.empty()) {
            err << error_message::config::data_dir_missing << '\n';
            return std::nullopt;
        }
        path = path.size() == 1 ? home : (std::filesystem::path{home} / path.substr(2)).string();
    }

    if (same_account) {
        for (const char* key : {"MAILAI_ACCOUNT_ID", "MAILAI_ACCOUNT_NAME"}) {
            if (const auto value = setting(defaults, key); !value.empty())
                config[key] = value;
        }
    }
    if (same_host) {
        if (auto ca_file = setting(defaults, "MAILAI_IMAP_CA_FILE"); !ca_file.empty())
            config["MAILAI_IMAP_CA_FILE"] = std::move(ca_file);
    }

    // 在询问密码之前检查全部非秘密配置；旧环境中的密码变量不进入登录配置。
    if (const auto validated = parse_sync_config(config); !validated) {
        print_error(err, validated.error());
        return std::nullopt;
    }
    return config;
}

void print_login_error(std::ostream& err, const Error& error, std::string_view password) {
    // 即使下层返回了包含密码的错误，也不把它打印到终端。
    if (error.message.find(password) != std::string::npos ||
        error.stage.find(password) != std::string::npos ||
        (error.account_id && error.account_id->find(password) != std::string::npos))
        err << error_message::login_cli::login_failed << " [code=" << error_code_name(error.code)
            << "]\n";
    else
        print_error(err, error);
}

}  // namespace

LoginInput terminal_login_input() {
#if defined(__APPLE__)
    if (!::isatty(STDIN_FILENO) || !::isatty(STDERR_FILENO))
        return {};
    return {
        [](std::string& line) { return static_cast<bool>(std::getline(std::cin, line)); },
        [](std::string& password) {
            PasswordBuffer buffer;
            if (!::readpassphrase("", buffer.value.data(), buffer.value.size(), RPP_REQUIRE_TTY))
                return false;
            const auto size = std::strlen(buffer.value.data());
            // readpassphrase 可能截断过长输入，拒绝恰好填满缓冲区的结果。
            if (size == 0 || size >= buffer.value.size() - 1)
                return false;
            password.assign(buffer.value.data(), size);
            return true;
        }
    };
#else
    return {};
#endif
}

int execute_login(
    const MailServices& services, std::ostream& out, std::ostream& err, const LoginInput& input) {
    if (!input.read_line || !input.read_password) {
        err << error_message::login_cli::terminal_required << '\n';
        return 1;
    }
    if (!services.login_defaults || !services.login) {
        err << error_message::login_cli::unavailable << '\n';
        return 1;
    }
    const auto defaults = services.login_defaults();
    if (!defaults) {
        print_error(err, defaults.error());
        return 1;
    }
    auto config = ask_configuration(*defaults, input, err);
    if (!config)
        return 1;

    Password password;
    err << "邮箱密码或授权码（输入隐藏）: " << std::flush;
    if (!input.read_password(password.value) || password.value.empty()) {
        err << error_message::login_cli::password_missing << '\n';
        return 1;
    }
    const auto result = services.login(*config, password.value);
    if (!result) {
        print_login_error(err, result.error(), password.value);
        return 1;
    }
    out << "登录成功。现在可以使用 sync 同步邮件。\n";
    return 0;
}

int execute_logout(const MailServices& services, std::ostream& out, std::ostream& err) {
    if (!services.logout) {
        err << error_message::login_cli::logout_unavailable << '\n';
        return 1;
    }
    const auto result = services.logout();
    if (!result) {
        print_error(err, result.error());
        return 1;
    }
    out << "已退出登录，本地邮件缓存已保留。\n";
    return 0;
}

}  // namespace mailai::cli

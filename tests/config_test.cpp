#include "mailai/config.h"
#include "test_support.h"

#include <filesystem>

namespace {

using namespace mailai;

Environment valid_sync_environment()
{
    return {
        {"MAILAI_DATA_DIR", "/tmp/mailai-test/sync"},
        {"MAILAI_EMAIL", "student@school.example"},
        {"MAILAI_IMAP_HOST", "imap.school.example"},
        {"MAILAI_IMAP_USER", "student"}
    };
}

void test_valid_sync_config()
{
    const auto env = valid_sync_environment();
    const auto result = parse_sync_config(env);

    expect(
        result.has_value(),
        "valid sync config should be accepted"
    );

    const auto& config = *result;

    expect(config.account.id == "school", "default account ID should be school");
    expect(config.account.display_name == "学校邮箱",
           "default account name should be the school mailbox name");
    expect(config.account.address == "student@school.example",
           "account address should come from MAILAI_EMAIL");
    expect(config.imap.host == "imap.school.example",
           "IMAP host should come from MAILAI_IMAP_HOST");
    expect(config.imap.username == "student",
           "IMAP username should come from MAILAI_IMAP_USER");
    expect(config.imap.tls_mode == TlsMode::implicit,
           "default TLS mode should be implicit");
    expect(config.imap.port == 993, "implicit TLS should default to port 993");
    expect(config.imap.credential_env == "MAILAI_IMAP_PASSWORD",
           "config should store the default credential variable name");
    expect(config.data_dir == std::filesystem::path{"/tmp/mailai-test/sync"},
           "sync config should preserve the resolved data directory");
}

void test_starttls_default_port()
{
    auto env = valid_sync_environment();
    env["MAILAI_IMAP_TLS"] = "starttls";

    const auto result = parse_sync_config(env);
    expect(result.has_value(),
           "STARTTLS sync config should be accepted");

    const auto& config = *result;
    expect(config.imap.tls_mode == TlsMode::starttls,
           "STARTTLS mode should be preserved");
    expect(config.imap.port == 143, "STARTTLS should default to port 143");
}

void test_custom_sync_config()
{
    auto env = valid_sync_environment();
    env["MAILAI_ACCOUNT_ID"] = "campus";
    env["MAILAI_ACCOUNT_NAME"] = "个人学校邮箱";
    env["MAILAI_PASSWORD_ENV"] = "MAILAI_TEST_PASSWORD";
    env["MAILAI_IMAP_TLS"] = "starttls";
    env["MAILAI_IMAP_PORT"] = "1143";

    const auto result = parse_sync_config(env);
    expect(result.has_value(),
           "custom sync config should be accepted without a password value");

    const auto& config = *result;
    expect(config.account.id == "campus", "explicit account ID should be preserved");
    expect(config.account.display_name == "个人学校邮箱",
           "explicit account name should be preserved");
    expect(config.imap.credential_env == "MAILAI_TEST_PASSWORD",
           "explicit credential variable name should be preserved");
    expect(config.imap.tls_mode == TlsMode::starttls,
           "explicit STARTTLS mode should be preserved");
    expect(config.imap.port == 1143, "explicit port should override the TLS default");
}

void test_explicit_port_boundaries()
{
    struct PortCase {
        const char* text;
        std::uint16_t expected;
    };
    const PortCase cases[]{
        {"1", 1},
        {"65535", 65535}
    };

    for (const auto& test : cases) {
        auto env = valid_sync_environment();
        env["MAILAI_IMAP_PORT"] = test.text;

        const auto result = parse_sync_config(env);
        expect(result.has_value(),
               "a port within the valid range should be accepted");
        expect(result->imap.port == test.expected,
               "explicit boundary port should be preserved without narrowing errors");
    }
}

void test_invalid_config()
{
    struct InvalidCase {
        const char* key;
        const char* value;
        const char* message;
    };
    const InvalidCase cases[]{
        {"MAILAI_EMAIL", nullptr, "missing email should be rejected"},
        {"MAILAI_EMAIL", "", "empty email should be rejected"},
        {"MAILAI_IMAP_HOST", nullptr, "missing IMAP host should be rejected"},
        {"MAILAI_IMAP_HOST", "", "empty IMAP host should be rejected"},
        {"MAILAI_IMAP_USER", nullptr, "missing IMAP username should be rejected"},
        {"MAILAI_IMAP_USER", "", "empty IMAP username should be rejected"},
        {"MAILAI_IMAP_TLS", "unknown", "unknown TLS mode should be rejected"},
        {"MAILAI_IMAP_PORT", "0", "port zero should be rejected"},
        {"MAILAI_IMAP_PORT", "65536", "port above 65535 should be rejected"},
        {"MAILAI_IMAP_PORT", "-1", "negative port should be rejected"},
        {"MAILAI_IMAP_PORT", "+993", "port with a leading plus should be rejected"},
        {"MAILAI_IMAP_PORT", "993x", "port with trailing text should be rejected"},
        {"MAILAI_IMAP_PORT", " 993", "port with leading whitespace should be rejected"},
        {"MAILAI_IMAP_PORT", "993 ", "port with trailing whitespace should be rejected"},
        {"MAILAI_IMAP_PORT", "999999999999999999999999", "overflowing port should be rejected"},
        {"MAILAI_DATA_DIR", nullptr, "sync config without any data directory source should be rejected"}
    };

    for (const auto& test : cases) {
        auto env = valid_sync_environment();
        if (test.value) {
            env[test.key] = test.value;
        } else {
            env.erase(test.key);
        }

        const auto result = parse_sync_config(env);
        expect(!result.has_value(), test.message);
        const auto& error = result.error();
        expect(error.code == ErrorCode::config_error,
               "invalid sync config should report config_error");
        expect(!error.message.empty(), "config errors should include a diagnostic");
    }
}

void test_missing_data_dir_sources()
{
    const auto result = resolve_data_dir(Environment{});

    expect(
        !result.has_value(),
        "missing data directory sources should return an error"
    );
}

void test_relative_xdg_data_home_is_ignored()
{
    Environment env{
        {"XDG_DATA_HOME", "relative-data"},
        {"HOME", "/tmp/mailai-test/home"}
    };

    auto result = resolve_data_dir(env);
    expect(result.has_value(),
           "relative XDG_DATA_HOME should not prevent the HOME fallback");
    expect(*result == std::filesystem::path{
               "/tmp/mailai-test/home/.local/share/mail-ai-assistant"},
           "relative XDG_DATA_HOME should be ignored in favor of HOME");

    env.erase("HOME");
    result = resolve_data_dir(env);
    expect(!result.has_value(),
           "relative XDG_DATA_HOME alone should not resolve a data directory");
    expect(result.error().code == ErrorCode::config_error,
           "no usable data directory source should report config_error");
}

void test_data_dir_precedence() {
    Environment env{
        {"MAILAI_DATA_DIR", "/tmp/mailai-test/custom"},
        {"XDG_DATA_HOME", "/tmp/mailai-test/xdg"},
        {"HOME", "/tmp/mailai-test/home"}
    };

    auto result = resolve_data_dir(env);

    expect(
        result.has_value(),
        "explicit data directory should be accepted"
    );

    expect(
        *result
            == std::filesystem::path{"/tmp/mailai-test/custom"},
        "explicit data directory should take precedence"
    );

    env.erase("MAILAI_DATA_DIR");
    result = resolve_data_dir(env);

    expect(
        result.has_value(),
        "XDG data directory should be accepted when no explicit directory is set"
    );

    expect(
        *result
            == std::filesystem::path{"/tmp/mailai-test/xdg/mail-ai-assistant"},
        "data directory should be mail-ai-assistant under XDG_DATA_HOME"
    );

    env.erase("XDG_DATA_HOME");
    result = resolve_data_dir(env);

    expect(
        result.has_value(),
        "HOME fallback should be accepted when no explicit or XDG directory is set"
    );

    expect(
        *result
            == std::filesystem::path{"/tmp/mailai-test/home/.local/share/mail-ai-assistant"},
        "data directory should be .local/share/mail-ai-assistant under HOME"
    );

    env.erase("HOME");
    result = resolve_data_dir(env);

    expect(
        !result.has_value(),
        "missing data directory sources should return an error"
    );

    expect(
        result.error().code == ErrorCode::config_error,
        "missing data directory sources should report config_error"
    );

    env = {
        {"MAILAI_DATA_DIR", ""},
        {"XDG_DATA_HOME", "/tmp/mailai-test/xdg"},
        {"HOME", "/tmp/mailai-test/home"}
    };
    result = resolve_data_dir(env);

    expect(
        result.has_value(),
        "empty explicit directory should fall back to XDG"
    );

    expect(
        *result
            == std::filesystem::path{"/tmp/mailai-test/xdg/mail-ai-assistant"},
        "XDG data directory should be used when the explicit directory is empty"
    );

    env["XDG_DATA_HOME"] = "";
    result = resolve_data_dir(env);

    expect(
        result.has_value(),
        "empty explicit and XDG directories should fall back to HOME"
    );

    expect(
        *result
            == std::filesystem::path{"/tmp/mailai-test/home/.local/share/mail-ai-assistant"},
        "HOME default data directory should be used when explicit and XDG directories are empty"
    );

    env["HOME"] = "";
    result = resolve_data_dir(env);

    expect(
        !result.has_value(),
        "empty data directory sources should return an error"
    );

    expect(
        result.error().code == ErrorCode::config_error,
        "empty data directory sources should report config_error"
    );
}

} // namespace

int main() {
    test_missing_data_dir_sources();
    test_data_dir_precedence();
    test_relative_xdg_data_home_is_ignored();
    test_valid_sync_config();
    test_starttls_default_port();
    test_custom_sync_config();
    test_explicit_port_boundaries();
    test_invalid_config();
}

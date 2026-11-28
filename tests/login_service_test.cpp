#include "app/login_service.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

#include "mailai/login_profile.h"
#include "mailai/store.h"
#include "store_test_support.h"

namespace {

using namespace mailai;
using namespace mailai::login_detail;
using store_test::TempDirectory;

struct TestCredentials {
    std::map<std::string, std::string> values;
    int reads{};
    int writes{};
    int erases{};
    std::function<void()> after_write;
    std::string reject_erase;

    CredentialStore store() {
        return {
            [&](std::string_view key) -> Result<std::optional<std::string>> {
                ++reads;
                if (auto it = values.find(std::string{key}); it != values.end())
                    return std::optional<std::string>{it->second};
                return std::optional<std::string>{};
            },
            [&](std::string_view key, std::string_view password) -> Status {
                ++writes;
                values[std::string{key}] = password;
                if (after_write)
                    after_write();
                return {};
            },
            [&](std::string_view key) -> Status {
                ++erases;
                if (key == reject_erase)
                    return std::unexpected(
                        Error{ErrorCode::storage_error, "Synthetic erase failure", {}, {}});
                values.erase(std::string{key});
                return {};
            }
        };
    }
};

Environment input_for(const std::filesystem::path& cache) {
    return {
        {"MAILAI_EMAIL", "student@example.test"},
        {"MAILAI_IMAP_HOST", "imap.example.test"},
        {"MAILAI_IMAP_USER", "student@example.test"},
        {"MAILAI_IMAP_PORT", "993"},
        {"MAILAI_IMAP_TLS", "implicit"},
        {"MAILAI_DATA_DIR", cache.string()}
    };
}

void test_login_remember_and_logout() {
    TempDirectory dir;
    const Environment env{{"HOME", dir.path().string()}};
    const auto cache = dir.path() / "old-cache";
    const auto input = input_for(cache);
    TestCredentials credentials;
    const auto keychain = credentials.store();
    const auto verify = [](const RuntimeConfig& config) -> Result<MailboxSnapshot> {
        expect(config.imap.host == "imap.example.test", "login verifies the selected server");
        expect(config.credential_provider && config.credential_provider() == "synthetic-secret",
            "login validates with the supplied password rather than ambient environment");
        return MailboxSnapshot{77, {}};
    };
    expect(remember_login(env, input, "synthetic-secret", keychain, verify).has_value(),
        "successful login saves settings and password");
    expect(credentials.writes == 1 && credentials.values.size() == 1,
        "login stores exactly one dedicated credential");
    const auto saved = load_login_profile(env);
    expect(saved && *saved && (**saved).at("MAILAI_DATA_DIR") == cache.string(),
        "remembered settings retain the chosen cache path");
    for (const auto& [key, value] : **saved)
        expect(value != "synthetic-secret", "profile must not contain the password");

    expect(resolve_cache_directory(env) == cache && credentials.reads == 0,
        "offline cache lookup remembers location without unlocking the keychain");
    auto config = resolve_sync_settings(env, keychain);
    expect(config && config->data_dir == cache && config->credential_provider &&
               config->credential_provider() == "synthetic-secret",
        "fresh process settings recover the saved credential");
    expect(credentials.reads == 1, "sync loads the credential only once");
    expect(forget_login(env, keychain).has_value() && credentials.values.empty(),
        "logout erases the remembered credential");
    expect(resolve_cache_directory(env) == cache,
        "logout leaves the original cache directory selected");
    expect(!resolve_sync_settings(env, keychain), "sync after logout requires login");
    expect(forget_login(env, keychain).has_value(), "logout is idempotent");
}

void test_failed_login_and_explicit_environment() {
    TempDirectory dir;
    const Environment env{{"HOME", dir.path().string()}};
    auto input = input_for(dir.path() / "cache");
    TestCredentials credentials;
    const auto keychain = credentials.store();
    const auto rejected = [](const RuntimeConfig&) -> Result<MailboxSnapshot> {
        return std::unexpected(Error{ErrorCode::auth_error, "Synthetic auth failure", {}, {}});
    };
    expect(!remember_login(env, input, "wrong-secret", keychain, rejected),
        "failed authentication cannot save a profile");
    const auto no_profile = load_login_profile(env);
    expect(no_profile && !*no_profile && credentials.writes == 0,
        "failed authentication never writes credential or profile");

    const auto good = [](const RuntimeConfig&) -> Result<MailboxSnapshot> {
        return MailboxSnapshot{77, {}};
    };
    expect(remember_login(env, input, "old-secret", keychain, good).has_value(),
        "first valid login succeeds");
    expect(!remember_login(env, input, "wrong-secret", keychain, rejected),
        "failed relogin preserves prior valid credentials");
    auto remembered = resolve_sync_settings(env, keychain);
    expect(remembered && remembered->credential_provider() == "old-secret",
        "failed relogin must not overwrite the old credential");

    auto explicit_env = env;
    explicit_env.merge(input_for(dir.path() / "different-cache"));
    explicit_env["MAILAI_IMAP_HOST"] = "different.example.test";
    explicit_env["MAILAI_PASSWORD_ENV"] = "FIXTURE_PASSWORD";
    const auto reads = credentials.reads;
    const auto explicit_config = resolve_sync_settings(explicit_env, keychain);
    expect(explicit_config && explicit_config->imap.host == "different.example.test" &&
               explicit_config->imap.credential_env == "FIXTURE_PASSWORD" &&
               !explicit_config->credential_provider && credentials.reads == reads,
        "explicit environment settings never combine a new server with remembered credentials");
    expect(resolve_cache_directory(explicit_env) == dir.path() / "different-cache",
        "explicit data directory takes precedence");
    auto partial = env;
    partial["MAILAI_IMAP_HOST"] = "different.example.test";
    expect(!resolve_sync_settings(partial, keychain) && credentials.reads == reads,
        "partial explicit server configuration must not fall back to a saved password");
}

void test_credential_rollback_and_alias() {
    TempDirectory dir;
    const Environment env{{"HOME", dir.path().string()}};
    const auto input = input_for(dir.path() / "cache");
    TestCredentials credentials;
    const auto keychain = credentials.store();
    const auto good = [](const RuntimeConfig&) -> Result<MailboxSnapshot> {
        return MailboxSnapshot{77, {}};
    };
    expect(remember_login(env, input, "first-secret", keychain, good).has_value(),
        "initial login succeeds");
    const auto old_key = credentials.values.begin()->first;
    const auto profile = *resolve_login_profile_dir(env) / "login.conf";
    credentials.after_write = [&] {
        std::filesystem::permissions(
            profile, std::filesystem::perms::group_read, std::filesystem::perm_options::add);
    };
    expect(!remember_login(env, input, "replacement", keychain, good),
        "profile publication failure must fail login");
    credentials.after_write = {};
    std::filesystem::permissions(
        profile, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    expect(credentials.values.size() == 1 && credentials.values.at(old_key) == "first-secret",
        "failed same-identity update restores previous credential");

    auto changed = input_for(dir.path() / "new-cache");
    changed["MAILAI_IMAP_HOST"] = "another.example.test";
    credentials.reject_erase = old_key;
    expect(!remember_login(env, changed, "replacement", keychain, good),
        "switch fails if old credential cannot be removed");
    expect(credentials.values.size() == 1 && credentials.values.at(old_key) == "first-secret",
        "failed switch does not orphan new credential");
    expect(resolve_cache_directory(env) == dir.path() / "cache",
        "failed switch preserves previous profile");
    credentials.reject_erase.clear();

    TempDirectory alias_dir;
    const auto alias = alias_dir.path() / "home-link";
    std::filesystem::create_directory_symlink(dir.path(), alias);
    const Environment alias_env{{"HOME", alias.string()}};
    auto alias_config = resolve_sync_settings(alias_env, keychain);
    expect(alias_config && alias_config->credential_provider() == "first-secret",
        "alias of the same profile resolves the same Keychain item");
    expect(forget_login(alias_env, keychain).has_value() && credentials.values.empty(),
        "logout through equivalent path removes the saved credential");
}

void test_existing_cache_identity() {
    TempDirectory dir;
    const Environment env{{"HOME", dir.path().string()}};
    auto input = input_for(dir.path());
    auto config = parse_sync_config(input);
    expect(config.has_value(), "fixture configuration");
    auto cache = SqliteStore::open(dir.path() / "cache.sqlite3", OpenMode::read_write_create);
    expect(cache && cache->ensure_account(config->account, config->imap).has_value(),
        "fixture existing cache");
    input["MAILAI_IMAP_HOST"] = "another.example.test";
    TestCredentials credentials;
    const auto unexpected_verification = [](const RuntimeConfig&) -> Result<MailboxSnapshot> {
        throw std::runtime_error("incompatible cache should fail before connecting");
    };
    expect(!remember_login(env, input, "synthetic", credentials.store(), unexpected_verification) &&
               credentials.writes == 0,
        "different account cannot reuse a conflicting local cache");
    const auto regular_file = dir.path() / "regular-file";
    std::ofstream{regular_file} << "not a directory";
    input["MAILAI_DATA_DIR"] = regular_file.string();
    expect(!remember_login(env, input, "synthetic", credentials.store(), unexpected_verification),
        "regular file cannot be a cache directory");
}

}  // namespace

int main() {
    try {
        test_login_remember_and_logout();
        test_failed_login_and_explicit_environment();
        test_credential_rollback_and_alias();
        test_existing_cache_identity();
        std::cout << "Login service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return 1;
    }
}

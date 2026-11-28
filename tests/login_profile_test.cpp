#include "mailai/login_profile.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "../src/util/secure_fs.h"
#include "mailai/services.h"
#include "mailai/store.h"
#include "test_support.h"

namespace {

using namespace mailai;
namespace fs = std::filesystem;

class TempDirectory {
public:
    TempDirectory() {
        auto pattern = (fs::temp_directory_path() / "mailai-profile-XXXXXX").string();
        expect(::mkdtemp(pattern.data()) != nullptr, "test directory should be created");
        path_ = std::move(pattern);
    }

    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const fs::path& path() const {
        return path_;
    }

private:
    fs::path path_;
};

Environment fixture_profile() {
    return {
        {"MAILAI_EMAIL", "student@school.example"},
        {"MAILAI_IMAP_HOST", "imap.school.example"},
        {"MAILAI_IMAP_USER", "student"},
        {"MAILAI_ACCOUNT_ID", "campus"},
        {"MAILAI_ACCOUNT_NAME", "Campus = Mail"},
        {"MAILAI_IMAP_CA_FILE", "/tmp/fixture ca.pem"},
        {"MAILAI_IMAP_TLS", "starttls"},
        {"MAILAI_IMAP_PORT", "1143"}
    };
}

fs::path profile_path(const Environment& env) {
    const auto dir = resolve_login_profile_dir(env);
    expect(dir.has_value(), "fixture should resolve a profile directory");
    return *dir / "login.conf";
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(input.good(), "profile fixture should be readable");
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_file(const fs::path& path, const std::string& contents, mode_t mode = 0600) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    output.close();
    expect(output.good() && ::chmod(path.c_str(), mode) == 0, "fixture should be writable");
}

void test_profile_directory_precedence() {
    Environment env{
        {"XDG_CONFIG_HOME", "/tmp/config root"},
        {"HOME", "/tmp/home"},
        {"MAILAI_DATA_DIR", "/tmp/data"},
        {"XDG_DATA_HOME", "/tmp/xdg-data"}
    };
    const auto xdg = resolve_login_profile_dir(env);
    expect(xdg && *xdg == fs::path{"/tmp/config root/mail-ai-assistant"},
        "absolute XDG_CONFIG_HOME should beat HOME and be independent of data paths");

    env["XDG_CONFIG_HOME"] = "relative-config";
    const auto fallback = resolve_login_profile_dir(env);
    expect(fallback && *fallback == fs::path{"/tmp/home/.config/mail-ai-assistant"},
        "relative XDG_CONFIG_HOME should fall back to HOME/.config");

    env.erase("HOME");
    expect(!resolve_login_profile_dir(env), "data directory must not be used as config directory");
    env["HOME"] = "relative-home";
    expect(!resolve_login_profile_dir(env), "relative HOME must not resolve a config directory");
}

void test_missing_profile_does_not_create_directory() {
    TempDirectory directory;
    const Environment env{{"HOME", directory.path().string()}};
    const auto missing = load_login_profile(env);
    expect(missing && !*missing, "a missing profile should return an empty optional");
    expect(
        !fs::exists(directory.path() / ".config"), "loading must not create profile directories");
}

void test_ensure_directory_creates_only_secure_config_directory() {
    TempDirectory directory;
    const Environment env{{"XDG_CONFIG_HOME", (directory.path() / "config root").string()}};
    expect(ensure_login_profile_dir(env).has_value(), "configuration directory should be created");
    const auto path = profile_path(env).parent_path();
    struct stat info{};
    expect(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700,
        "ensuring the config directory must create it with owner-only permissions");
    expect(fs::is_empty(path), "ensuring the config directory must not write a profile");
    const auto missing = load_login_profile(env);
    expect(
        missing && !*missing, "a secure empty configuration directory has no remembered profile");
    expect(ensure_login_profile_dir(env).has_value(), "directory creation must be idempotent");
    expect(::chmod(path.c_str(), 0755) == 0, "fixture permissions should change");
    expect(!ensure_login_profile_dir(env),
        "an existing unsafe directory must not be silently accepted");
}

void test_round_trip_keeps_only_nonsecret_fields() {
    TempDirectory directory;
    const Environment env{
        {"HOME", directory.path().string()},
        {"MAILAI_DATA_DIR", (directory.path() / "mail cache").string()}
    };
    auto profile = fixture_profile();
    profile["MAILAI_PASSWORD_ENV"] = "TEST_REF";
    profile["MAILAI_IMAP_PASSWORD"] = "fixture-only-password";
    profile["TEST_REF"] = "fixture-only-reference-value";
    profile["HOME"] = "/tmp/not-the-config-dir";
    expect(save_login_profile(env, profile).has_value(), "profile should save without credentials");

    const auto path = profile_path(env);
    const auto contents = read_file(path);
    expect(contents.find("fixture-only") == std::string::npos &&
               contents.find("MAILAI_PASSWORD_ENV") == std::string::npos &&
               contents.find("TEST_REF") == std::string::npos &&
               contents.find("HOME=") == std::string::npos,
        "serialized profile must not include credentials, credential references or arbitrary keys");

    auto expected = fixture_profile();
    expected["MAILAI_DATA_DIR"] = (directory.path() / "mail cache").string();
    const auto loaded = load_login_profile(env);
    expect(loaded && *loaded && **loaded == expected,
        "round-trip should return only the allowed values, including spaces and equals signs");

    struct stat file_info{};
    struct stat dir_info{};
    expect(::stat(path.c_str(), &file_info) == 0 && (file_info.st_mode & 0777) == 0600,
        "profile file must be readable and writable only by its owner");
    expect(::stat(path.parent_path().c_str(), &dir_info) == 0 && (dir_info.st_mode & 0777) == 0700,
        "profile directory must be accessible only by its owner");
}

void test_resolves_relative_data_directory_when_saving() {
    TempDirectory directory;
    const Environment env{{"HOME", directory.path().string()}};
    auto profile = fixture_profile();
    profile["MAILAI_DATA_DIR"] = "relative cache";
    expect(
        save_login_profile(env, profile).has_value(), "relative data path should be made absolute");
    const auto loaded = load_login_profile(env);
    expect(loaded && *loaded, "saved profile should reload");
    expect((**loaded).at("MAILAI_DATA_DIR") == (fs::current_path() / "relative cache").string(),
        "remembered data path must not depend on the next invocation's current directory");
}

void test_replaces_profile_and_preserves_previous_on_invalid_input() {
    TempDirectory directory;
    const Environment env{{"HOME", directory.path().string()}};
    auto profile = fixture_profile();
    expect(save_login_profile(env, profile).has_value(), "initial profile should save");
    const auto path = profile_path(env);
    struct stat previous{};
    expect(::stat(path.c_str(), &previous) == 0, "previous profile should exist");

    profile["MAILAI_ACCOUNT_NAME"] = "New name";
    expect(save_login_profile(env, profile).has_value(), "valid profile should be replaceable");
    struct stat current{};
    expect(::stat(path.c_str(), &current) == 0 && previous.st_ino != current.st_ino,
        "profile updates must replace the file rather than truncating an existing inode");
    const auto saved = read_file(path);

    for (const auto& invalid :
        {std::string{"name\nvalue"}, std::string{"name\rvalue"}, std::string{"name\0value", 10}}) {
        auto malformed = profile;
        malformed["MAILAI_ACCOUNT_NAME"] = invalid;
        expect(!save_login_profile(env, malformed), "invalid value must not be serialized");
        expect(read_file(path) == saved, "invalid update must preserve previous profile");
    }
    profile["MAILAI_IMAP_PORT"] = "65536";
    expect(!save_login_profile(env, profile), "invalid synchronization config must not be saved");
    expect(read_file(path) == saved, "invalid sync config must preserve previous profile");
    expect(std::distance(fs::directory_iterator(path.parent_path()), fs::directory_iterator{}) == 1,
        "successful replacement and rejected updates must not leave temporary files");
}

void test_malformed_profile_is_rejected() {
    TempDirectory directory;
    const Environment env{{"HOME", directory.path().string()}};
    expect(save_login_profile(env, fixture_profile()).has_value(), "initial profile should save");
    const auto path = profile_path(env);
    const auto valid = read_file(path);

    for (const auto& line :
        {std::string{"MAILAI_EMAIL=duplicate@example.test\n"},
            std::string{"MAILAI_IMAP_PASSWORD=fixture-only-password\n"},
            std::string{"OTHER=unexpected\n"},
            std::string{"MISSING_EQUALS\n"},
            std::string{"=empty-key\n"},
            std::string{"\n"},
            std::string{"MAILAI_ACCOUNT_NAME=x\r\n"},
            std::string{"BAD\0=value\n", 11}}) {
        write_file(path, valid + line);
        const auto result = load_login_profile(env);
        expect(!result, "malformed or unapproved profile lines must be rejected");
        expect(result.error().message.find("fixture-only-password") == std::string::npos,
            "profile error must not quote rejected data");
    }

    write_file(path,
        "MAILAI_EMAIL=student@school.example\nMAILAI_IMAP_HOST=imap.school.example\n"
        "MAILAI_IMAP_USER=student\nMAILAI_DATA_DIR=relative\n");
    expect(!load_login_profile(env), "a remembered relative data directory must be rejected");
    write_file(path, "MAILAI_DATA_DIR=/tmp/fixture\n");
    expect(
        !load_login_profile(env), "a remembered profile with incomplete configuration must fail");
}

void test_oversized_profile_is_rejected() {
    TempDirectory directory;
    const Environment env{{"HOME", directory.path().string()}};
    auto profile = fixture_profile();
    expect(save_login_profile(env, profile).has_value(), "initial profile should save");
    const auto path = profile_path(env);
    const auto valid = read_file(path);

    profile["MAILAI_ACCOUNT_NAME"] = std::string(64 * 1024, 'x');
    expect(!save_login_profile(env, profile),
        "oversized settings must not replace an existing profile");
    expect(read_file(path) == valid, "oversized update must preserve the existing profile");
    write_file(path,
        "MAILAI_EMAIL=student@school.example\nMAILAI_IMAP_HOST=imap.school.example\n"
        "MAILAI_IMAP_USER=student\nMAILAI_DATA_DIR=/tmp/fixture\nMAILAI_ACCOUNT_NAME=" +
            std::string(64 * 1024, 'x') + "\n");
    expect(!load_login_profile(env), "oversized profile files must be rejected");
}

void test_insecure_file_or_directory_is_not_used() {
    TempDirectory directory;
    const Environment env{{"HOME", directory.path().string()}};
    auto profile = fixture_profile();
    expect(save_login_profile(env, profile).has_value(), "initial profile should save");
    const auto path = profile_path(env);
    const auto contents = read_file(path);

    expect(::chmod(path.c_str(), 0644) == 0, "profile fixture permissions should change");
    expect(!load_login_profile(env) && !save_login_profile(env, profile),
        "insecure profile permissions must reject reading and replacement");
    expect(::chmod(path.c_str(), 0600) == 0, "profile fixture permissions should restore");
    expect(::chmod(path.parent_path().c_str(), 0755) == 0, "directory permissions should change");
    expect(!load_login_profile(env) && !save_login_profile(env, profile),
        "insecure directory permissions must reject reading and writing");
    expect(::chmod(path.parent_path().c_str(), 0700) == 0, "directory permissions should restore");

    const auto elsewhere = directory.path() / "other-file";
    write_file(elsewhere, contents);
    fs::remove(path);
    fs::create_symlink(elsewhere, path);
    expect(!load_login_profile(env) && !save_login_profile(env, profile),
        "profile symlink must not be read or replaced");
    expect(fs::is_symlink(path) && read_file(elsewhere) == contents,
        "rejecting an unsafe profile must preserve the symlink and its target");

    fs::remove(path);
    fs::create_hard_link(elsewhere, path);
    expect(!load_login_profile(env) && !save_login_profile(env, profile),
        "multiply linked profile must not be read or replaced");
    fs::remove(path);

    expect(::mkfifo(path.c_str(), 0600) == 0, "FIFO fixture should be created");
    expect(!load_login_profile(env) && !save_login_profile(env, profile),
        "non-regular profile must be rejected without blocking");
    fs::remove(path);

    const auto redirect_dir = directory.path() / "directory-target";
    fs::create_directory(redirect_dir);
    expect(
        ::chmod(redirect_dir.c_str(), 0700) == 0, "redirect directory permissions should change");
    fs::remove(path.parent_path());
    fs::create_directory_symlink(redirect_dir, path.parent_path());
    expect(!load_login_profile(env) && !save_login_profile(env, profile),
        "symlink profile directory must not be read or populated");
    expect(fs::is_empty(redirect_dir), "rejecting the directory must leave its target untouched");
}

void test_open_at_only_accepts_a_single_filename() {
    TempDirectory directory;
    const auto fd = secure_fs::open_directory(directory.path(), secure_fs::FollowLink::no);
    expect(fd.has_value(), "open the fixture directory");
    for (const auto invalid :
        {std::string_view{},
            std::string_view{"."},
            std::string_view{".."},
            std::string_view{"a/b"},
            std::string_view{"file\0suffix", 11}}) {
        const auto file = secure_fs::open_at(fd->get(), invalid, O_RDONLY | O_CREAT, 0600);
        expect(!file && file.error() == std::errc::invalid_argument,
            "reject malformed names before calling openat");
    }
    expect(fs::is_empty(directory.path()), "invalid file names must leave the directory empty");
    expect(secure_fs::open_at(fd->get(), "file", O_RDONLY | O_CREAT, 0600).has_value(),
        "a valid direct child can be created");
    expect(!secure_fs::sync_directory(fd->get()), "the created directory entry can be synced");
}

void test_cache_rejects_database_symlinks() {
    TempDirectory directory;
    const auto root = fs::canonical(directory.path());
    const auto cache = root / "cache";
    fs::create_directory(cache);
    const auto database = cache / "cache.sqlite3";
    expect(SqliteStore::open(database, OpenMode::read_write_create).has_value(),
        "create a valid fixture database");

    const auto alias = root / "cache-alias";
    fs::create_directory_symlink(cache, alias);
    const auto services = make_runtime_services({{"MAILAI_DATA_DIR", alias.string()}});
    expect(services.list(DataSource::cache, {}).has_value(),
        "a directory alias should still resolve to the verified cache");

    const auto moved = root / "other.sqlite3";
    fs::rename(database, moved);
    fs::create_symlink(moved, database);
    expect(!services.list(DataSource::cache, {}), "read-only cache lookup should reject symlinks");
    expect(!SqliteStore::open(database, OpenMode::read_only) &&
               !SqliteStore::open(database, OpenMode::read_write_create),
        "SQLite should reject a symbolic link in both open modes");

    fs::remove(database);
    fs::rename(moved, database);
    const auto wal = cache / "cache.sqlite3-wal";
    fs::remove(wal);
    fs::create_symlink(database, wal);
    expect(!services.list(DataSource::cache, {}), "read-only lookup should reject WAL symlinks");
}

}  // namespace

int main() {
    test_profile_directory_precedence();
    test_missing_profile_does_not_create_directory();
    test_ensure_directory_creates_only_secure_config_directory();
    test_round_trip_keeps_only_nonsecret_fields();
    test_resolves_relative_data_directory_when_saving();
    test_replaces_profile_and_preserves_previous_on_invalid_input();
    test_malformed_profile_is_rejected();
    test_oversized_profile_is_rejected();
    test_insecure_file_or_directory_is_not_used();
    test_open_at_only_accepts_a_single_filename();
    test_cache_rejects_database_symlinks();
}

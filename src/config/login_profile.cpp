#include "mailai/login_profile.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <random>
#include <string_view>
#include <system_error>
#include <utility>

#include "mailai/error_message.h"
#include "util/secure_fs.h"

namespace mailai {

namespace {

namespace fs = std::filesystem;
namespace message = error_message::profile;

constexpr char kProfileFilename[] = "login.conf";
constexpr std::size_t kMaxProfileBytes = 64 * 1024;

constexpr std::array<std::string_view, 9> kProfileFields{
    "MAILAI_EMAIL",
    "MAILAI_IMAP_HOST",
    "MAILAI_IMAP_USER",
    "MAILAI_ACCOUNT_ID",
    "MAILAI_ACCOUNT_NAME",
    "MAILAI_IMAP_TLS",
    "MAILAI_IMAP_PORT",
    "MAILAI_IMAP_CA_FILE",
    "MAILAI_DATA_DIR",
};

Error profile_error(const char* detail) {
    return {ErrorCode::config_error, detail, std::nullopt, std::nullopt, "config.profile"};
}

bool allowed_field(std::string_view field) {
    return std::ranges::find(kProfileFields, field) != kProfileFields.end();
}

bool valid_value(std::string_view value) {
    constexpr std::string_view invalid_bytes{"\r\n\0", 3};
    return value.find_first_of(invalid_bytes) == std::string_view::npos;
}

using secure_fs::UniqueFd;
using FileDescriptor = UniqueFd;

bool secure_directory(const struct stat& info) {
    return S_ISDIR(info.st_mode) && secure_fs::owned_by_current_user(info) &&
           (info.st_mode & 07777) == 0700;
}

bool secure_file(const struct stat& info) {
    return S_ISREG(info.st_mode) && secure_fs::owned_by_current_user(info) &&
           (info.st_mode & 07777) == 0600 && info.st_nlink == 1;
}

// 缺少目录时，读取不创建目录，而是返回空文件描述符。
Result<FileDescriptor> open_profile_directory(const Environment& env, bool create) {
    const auto directory = resolve_login_profile_dir(env);
    if (!directory)
        return std::unexpected(directory.error());

    if (create) {
        std::error_code error;
        fs::create_directories(directory->parent_path(), error);
        if (error || (::mkdir(directory->c_str(), 0700) != 0 && errno != EEXIST)) {
            return std::unexpected(profile_error(message::directory_create_failed));
        }
    }

    auto fd = secure_fs::open_directory(*directory, secure_fs::FollowLink::no);
    if (!fd) {
        if (!create && fd.error() == std::errc::no_such_file_or_directory)
            return FileDescriptor{};
        return std::unexpected(profile_error(message::directory_unsafe));
    }

    const auto info = secure_fs::stat_fd(fd->get());
    if (!info || !secure_directory(*info)) {
        return std::unexpected(profile_error(message::directory_unsafe));
    }
    return std::move(*fd);
}

// 所有文件操作通过已经检查过的目录描述符完成，避免再次跟随目录路径。
Result<bool> profile_exists(int directory_fd) {
    struct stat info{};
    if (::fstatat(directory_fd, kProfileFilename, &info, AT_SYMLINK_NOFOLLOW) == 0) {
        if (secure_file(info))
            return true;
    } else if (errno == ENOENT) {
        return false;
    }
    return std::unexpected(profile_error(message::file_unsafe));
}

Result<std::string> read_profile(int directory_fd) {
    auto fd = secure_fs::open_at(directory_fd, kProfileFilename, O_RDONLY | O_NONBLOCK);
    if (!fd)
        return std::unexpected(profile_error(message::file_unsafe));
    const auto info = secure_fs::stat_fd(fd->get());
    if (!info || !secure_file(*info)) {
        return std::unexpected(profile_error(message::file_unsafe));
    }
    if (info->st_size > static_cast<off_t>(kMaxProfileBytes)) {
        return std::unexpected(profile_error(message::profile_too_large));
    }

    std::string contents;
    std::array<char, 4096> buffer;
    while (true) {
        const auto count = ::read(fd->get(), buffer.data(), buffer.size());
        if (count == 0)
            return contents;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(profile_error(message::read_failed));
        }
        if (static_cast<std::size_t>(count) > kMaxProfileBytes - contents.size()) {
            return std::unexpected(profile_error(message::profile_too_large));
        }
        contents.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

Status validate_profile(const Environment& profile) {
    const auto data_dir = profile.find("MAILAI_DATA_DIR");
    if (data_dir == profile.end() || !fs::path(data_dir->second).is_absolute()) {
        return std::unexpected(profile_error(message::absolute_data_dir_failed));
    }
    if (const auto config = parse_sync_config(profile); !config) {
        return std::unexpected(config.error());
    }
    return {};
}

Result<Environment> parse_profile(std::string_view contents) {
    Environment profile;
    while (!contents.empty()) {
        const auto end = contents.find('\n');
        const auto line = contents.substr(0, end);
        contents.remove_prefix(end == std::string_view::npos ? contents.size() : end + 1);

        const auto separator = line.find('=');
        if (separator == std::string_view::npos || !valid_value(line)) {
            return std::unexpected(profile_error(message::malformed));
        }
        const auto field = line.substr(0, separator);
        if (!allowed_field(field) || !profile.emplace(field, line.substr(separator + 1)).second) {
            return std::unexpected(profile_error(message::malformed));
        }
    }
    if (const auto status = validate_profile(profile); !status) {
        return std::unexpected(status.error());
    }
    return profile;
}

Result<std::string> serialize_profile(const Environment& env, const Environment& input) {
    Environment profile;
    for (const auto& [field, value] : input) {
        if (!allowed_field(field))
            continue;
        if (!valid_value(value))
            return std::unexpected(profile_error(message::invalid_value));
        profile.emplace(field, value);
    }

    // 数据目录只在保存时解析一次，之后从保存的绝对路径读取。
    auto directory_env = env;
    if (const auto it = profile.find("MAILAI_DATA_DIR"); it != profile.end()) {
        directory_env.insert_or_assign(it->first, it->second);
    }
    const auto data_dir = resolve_data_dir(directory_env);
    if (!data_dir)
        return std::unexpected(data_dir.error());
    if (!valid_value(data_dir->native())) {
        return std::unexpected(profile_error(message::invalid_value));
    }
    std::error_code error;
    const auto absolute_dir = fs::absolute(*data_dir, error);
    if (error || !absolute_dir.is_absolute() || !valid_value(absolute_dir.native())) {
        return std::unexpected(profile_error(message::absolute_data_dir_failed));
    }
    profile["MAILAI_DATA_DIR"] = absolute_dir.string();

    if (const auto status = validate_profile(profile); !status) {
        return std::unexpected(status.error());
    }

    std::string contents;
    for (const auto& [field, value] : profile) {
        if (value.size() > kMaxProfileBytes - contents.size() ||
            field.size() + 2 > kMaxProfileBytes - contents.size() - value.size()) {
            return std::unexpected(profile_error(message::profile_too_large));
        }
        contents += field;
        contents += '=';
        contents += value;
        contents += '\n';
    }
    return contents;
}

bool write_all(int fd, std::string_view contents) {
    while (!contents.empty()) {
        const auto written = ::write(fd, contents.data(), contents.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        contents.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

class TemporaryProfile {
public:
    TemporaryProfile(int directory_fd, const std::string& name) noexcept
        : directory_fd_(directory_fd), name_(name) {}
    ~TemporaryProfile() {
        if (active_)
            ::unlinkat(directory_fd_, name_.c_str(), 0);
    }
    TemporaryProfile(const TemporaryProfile&) = delete;
    TemporaryProfile& operator=(const TemporaryProfile&) = delete;

    void release() noexcept {
        active_ = false;
    }

private:
    int directory_fd_;
    const std::string& name_;
    bool active_{true};
};

std::string temporary_name() {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    return ".login.conf.tmp." + std::to_string(generator()) + "." + std::to_string(generator());
}

Status write_profile(int directory_fd, const std::string& contents) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::string name;
        try {
            name = temporary_name();
        } catch (const std::exception&) {
            return std::unexpected(profile_error(message::write_failed));
        }
        auto fd = secure_fs::open_at(directory_fd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (!fd) {
            if (fd.error() == std::errc::file_exists)
                continue;
            break;
        }

        TemporaryProfile temporary(directory_fd, name);
        if (::fchmod(fd->get(), 0600) != 0 || !write_all(fd->get(), contents) ||
            ::fsync(fd->get()) != 0 || fd->close() != 0) {
            break;
        }
        // 临时文件完整写入后再替换，失败时旧配置保持不变。
        if (::renameat(directory_fd, name.c_str(), directory_fd, kProfileFilename) != 0)
            break;
        temporary.release();
        // 新配置已经生效，目录落盘失败只报告脱敏诊断，避免调用方回滚其对应的凭据。
        if (const auto error = secure_fs::sync_directory(directory_fd)) {
            std::fprintf(stderr,
                "Warning: fsync(login profile directory) failed (errno %d)\n",
                error.value());
        }
        return {};
    }
    return std::unexpected(profile_error(message::write_failed));
}

}  // namespace

Result<std::filesystem::path> resolve_login_profile_dir(const Environment& env) {
    if (const auto it = env.find("XDG_CONFIG_HOME"); it != env.end()) {
        const fs::path xdg_dir(it->second);
        if (xdg_dir.is_absolute()) {
            if (!valid_value(it->second))
                return std::unexpected(profile_error(message::invalid_value));
            return xdg_dir / "mail-ai-assistant";
        }
    }
    if (const auto it = env.find("HOME"); it != env.end()) {
        const fs::path home(it->second);
        if (home.is_absolute() && valid_value(it->second)) {
            return home / ".config" / "mail-ai-assistant";
        }
    }
    return std::unexpected(profile_error(message::directory_missing));
}

Status ensure_login_profile_dir(const Environment& env) {
    const auto directory = open_profile_directory(env, true);
    if (!directory)
        return std::unexpected(directory.error());
    return {};
}

Result<std::optional<Environment>> load_login_profile(const Environment& env) {
    const auto directory = open_profile_directory(env, false);
    if (!directory)
        return std::unexpected(directory.error());
    if (directory->get() < 0)
        return std::optional<Environment>{};

    const auto exists = profile_exists(directory->get());
    if (!exists)
        return std::unexpected(exists.error());
    if (!*exists)
        return std::optional<Environment>{};

    const auto contents = read_profile(directory->get());
    if (!contents)
        return std::unexpected(contents.error());
    auto profile = parse_profile(*contents);
    if (!profile)
        return std::unexpected(profile.error());
    return std::optional<Environment>{std::move(*profile)};
}

Status save_login_profile(const Environment& env, const Environment& profile) {
    const auto contents = serialize_profile(env, profile);
    if (!contents)
        return std::unexpected(contents.error());

    const auto directory = open_profile_directory(env, true);
    if (!directory)
        return std::unexpected(directory.error());
    const auto exists = profile_exists(directory->get());
    if (!exists)
        return std::unexpected(exists.error());
    return write_profile(directory->get(), *contents);
}

}  // namespace mailai

#pragma once

#include <sys/stat.h>

#include <expected>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace mailai::secure_fs {

// 独占 POSIX 文件描述符，析构时关闭。
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd();

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const noexcept {
        return fd_;
    }
    explicit operator bool() const noexcept {
        return fd_ >= 0;
    }
    // 显式关闭并返回 close 的结果，便于写入后检查错误。
    int close() noexcept;

private:
    int fd_{-1};
};

enum class FollowLink { no, yes };

using FdResult = std::expected<UniqueFd, std::error_code>;

// 打开目录描述符；FollowLink::no 时拒绝最后一级为符号链接的路径。EINTR 自动重试。
FdResult open_directory(const std::filesystem::path& path, FollowLink follow);

// 只打开已验证目录的直接子项，不跟随符号链接。拒绝空名、斜杠、NUL、. 和 ..。
// EINTR 自动重试。
FdResult open_at(int directory_fd, std::string_view name, int flags, mode_t mode = 0);

// 读取描述符的状态信息。
std::expected<struct stat, std::error_code> stat_fd(int fd);

// 文件或目录属于当前有效用户。
bool owned_by_current_user(const struct stat& info) noexcept;

// 将目录项变更（创建、重命名）落盘。EINTR 自动重试。
std::error_code sync_directory(int directory_fd) noexcept;

}  // namespace mailai::secure_fs

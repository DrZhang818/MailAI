#include "secure_fs.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <utility>

namespace mailai::secure_fs {
namespace {

std::error_code last_error() noexcept {
    return {errno, std::generic_category()};
}

template <class Open>
FdResult open_retrying(Open open) {
    int fd;
    do {
        fd = open();
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return std::unexpected(last_error());
    return UniqueFd{fd};
}

}  // namespace

UniqueFd::~UniqueFd() {
    close();
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

int UniqueFd::close() noexcept {
    return fd_ < 0 ? 0 : ::close(std::exchange(fd_, -1));
}

FdResult open_directory(const std::filesystem::path& path, FollowLink follow) {
    const int flags =
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | (follow == FollowLink::no ? O_NOFOLLOW : 0);
    return open_retrying([&] { return ::open(path.c_str(), flags); });
}

FdResult open_at(int directory_fd, std::string_view name, int flags, mode_t mode) {
    if (name.empty() || name == "." || name == ".." || name.contains('/') || name.contains('\0')) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    const std::string filename{name};
    return open_retrying([&] {
        return ::openat(directory_fd, filename.c_str(), flags | O_NOFOLLOW | O_CLOEXEC, mode);
    });
}

std::expected<struct stat, std::error_code> stat_fd(int fd) {
    struct stat info{};
    if (::fstat(fd, &info) != 0)
        return std::unexpected(last_error());
    return info;
}

bool owned_by_current_user(const struct stat& info) noexcept {
    return info.st_uid == ::geteuid();
}

std::error_code sync_directory(int directory_fd) noexcept {
    int result;
    do {
        result = ::fsync(directory_fd);
    } while (result != 0 && errno == EINTR);
    if (result != 0)
        return last_error();
    return {};
}

}  // namespace mailai::secure_fs

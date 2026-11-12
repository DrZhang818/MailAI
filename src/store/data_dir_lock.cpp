#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "mailai/data_dir_lock.h"

namespace mailai {
namespace {

Error lock_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message), std::nullopt, std::nullopt, "storage.lock"};
}

} // namespace

DataDirLock::~DataDirLock() {
    if(fd_ >= 0) ::close(fd_);
}

DataDirLock::DataDirLock(DataDirLock&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

DataDirLock& DataDirLock::operator=(DataDirLock&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

Result<DataDirLock> DataDirLock::acquire(const std::filesystem::path& data_dir, std::chrono::milliseconds wait) {
    if (data_dir.empty() || data_dir.native().find('\0') != std::string::npos || wait.count() < 0) {
        return std::unexpected(
            lock_error(ErrorCode::invalid_argument, "Invalid data-directory lock settings")
        );
    }

    const auto path = data_dir / ".mailai.lock";

    int fd;
    do {
        fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        return std::unexpected(
            lock_error(
                ErrorCode::storage_error,
                "Cannot open data-directory lock: " +
                    std::error_code{errno, std::generic_category()}.message()
            )
        );
    }

    DataDirLock lock{fd};
    const auto started = std::chrono::steady_clock::now();

    while (true) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return lock;

        const int error = errno;
        if (error != EWOULDBLOCK && error != EAGAIN && error != EINTR) {
            return std::unexpected(
                lock_error(
                    ErrorCode::storage_error,
                    "Cannot acquire data-directory lock: " +
                        std::error_code{error, std::generic_category()}.message()
                )
            );
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        );

        if (elapsed >= wait) {
            return std::unexpected(
                lock_error(ErrorCode::busy, "Data directory is busy")
            );
        }

        std::this_thread::sleep_for(
            std::min(std::chrono::milliseconds{50}, wait - elapsed)
        );
    }
}

} // namespace mailai
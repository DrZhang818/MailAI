#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>
#include <utility>

#include "mailai/data_dir_lock.h"
#include "mailai/store.h"
#include "store_test_support.h"

namespace {
using namespace mailai;
using namespace store_test;
using namespace std::chrono_literals;

void expect_error(const Result<DataDirLock>& result, ErrorCode code) {
    expect(!result && result.error().code == code, "lock acquisition should return the expected error");
    expect(result.error().stage == "storage.lock" && !result.error().account_id &&
               !result.error().mail_id && !result.error().message.empty(),
        "lock errors should identify the operation without inventing a mail or account");
}

void close_fd(int& fd) {
    if (fd >= 0) ::close(std::exchange(fd, -1));
}

// The child acquires its own descriptor after fork, so the parent inherits no lock reference.
class LockHolder {
public:
    explicit LockHolder(const std::filesystem::path& directory) {
        if (::pipe(commands_) != 0 || ::pipe(status_) != 0) {
            cleanup();
            throw std::runtime_error("could not create process-control pipes");
        }

        pid_ = ::fork();
        if (pid_ < 0) {
            cleanup();
            throw std::runtime_error("could not start lock holder");
        }

        if (pid_ == 0) {
            ::close(commands_[1]);
            ::close(status_[0]);
            auto lock = DataDirLock::acquire(directory, 0ms);
            const char ready = lock ? 'R' : 'E';
            if (::write(status_[1], &ready, 1) != 1 || !lock) ::_exit(2);

            char command{};
            while (::read(commands_[0], &command, 1) < 0 && errno == EINTR) {}
            // Skip C++ destructors deliberately: process exit must also release the lock.
            ::_exit(0);
        }

        close_fd(commands_[0]);
        close_fd(status_[1]);
        pollfd ready_fd{status_[0], POLLIN, 0};
        char ready{};
        if (::poll(&ready_fd, 1, 2000) <= 0 || ::read(status_[0], &ready, 1) != 1 || ready != 'R') {
            cleanup();
            throw std::runtime_error("child process could not acquire the directory lock");
        }
    }

    ~LockHolder() { cleanup(); }
    LockHolder(const LockHolder&) = delete;
    LockHolder& operator=(const LockHolder&) = delete;

    void stop() {
        const char command = 'Q';
        expect(::write(commands_[1], &command, 1) == 1, "lock holder should receive its exit command");
        int status{};
        pid_t waited;
        do { waited = ::waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
        expect(waited == pid_ && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "lock holder should exit successfully");
        pid_ = -1;
        close_fd(commands_[1]);
        close_fd(status_[0]);
    }

private:
    void cleanup() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
            pid_ = -1;
        }
        close_fd(commands_[0]);
        close_fd(commands_[1]);
        close_fd(status_[0]);
        close_fd(status_[1]);
    }

    pid_t pid_{-1};
    int commands_[2]{-1, -1};
    int status_[2]{-1, -1};
};

void test_scope_moves_and_file_preservation() {
    TempDirectory directory;
    const auto lock_path = directory.path() / ".mailai.lock";
    const auto data_path = directory.path() / "existing-data";
    { std::ofstream output(lock_path); output << "existing lock-file content"; }
    { std::ofstream output(data_path); output << "preserve existing data"; }
    struct stat before{};
    expect(::stat(lock_path.c_str(), &before) == 0, "existing lock file should be readable");

    std::optional<DataDirLock> owner;
    {
        auto acquired = DataDirLock::acquire(directory.path());
        expect(acquired.has_value(), "a free directory should be lockable");
        owner.emplace(std::move(*acquired));
    }
    expect_error(DataDirLock::acquire(directory.path(), 0ms), ErrorCode::busy);
    owner.reset();
    expect(DataDirLock::acquire(directory.path(), 0ms).has_value(),
        "destroying the owner should release the lock");

    struct stat after{};
    expect(::stat(lock_path.c_str(), &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino,
        "release must preserve the same lock file so processes cannot lock separate inodes");
    expect(read_file(lock_path) == "existing lock-file content" && read_file(data_path) == "preserve existing data",
        "acquiring and releasing must not truncate the lock file or alter existing data");
}

void test_move_assignment_transfers_ownership() {
    TempDirectory first_directory;
    TempDirectory second_directory;
    {
        auto first  = DataDirLock::acquire(first_directory.path(), 0ms);
        auto second = DataDirLock::acquire(second_directory.path(), 0ms);
        expect(first && second, "different directories should lock independently");

        *first = std::move(*second);
        expect(DataDirLock::acquire(first_directory.path(), 0ms).has_value(),
            "move assignment should release the destination's previous lock");
        expect_error(DataDirLock::acquire(second_directory.path(), 0ms), ErrorCode::busy);
    }
    expect(DataDirLock::acquire(second_directory.path(), 0ms).has_value(),
        "the moved owner should release its adopted lock");
}

void test_process_contention_timeout_and_exit_recovery() {
    TempDirectory directory;
    TempDirectory other_directory;
    LockHolder holder(directory.path());

    expect(DataDirLock::acquire(other_directory.path(), 0ms).has_value(),
        "another process's lock must not block a different directory");
    expect_error(DataDirLock::acquire(directory.path(), 0ms), ErrorCode::busy);

    const auto started = std::chrono::steady_clock::now();
    expect_error(DataDirLock::acquire(directory.path(), 120ms), ErrorCode::busy);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(elapsed >= 100ms && elapsed < 1500ms, "a bounded wait should expire near its requested timeout");

    holder.stop();
    expect(DataDirLock::acquire(directory.path(), 0ms).has_value(),
        "the operating system should release a lock even when child destructors did not run");
}

void test_wait_succeeds_after_another_process_releases() {
    TempDirectory directory;
    LockHolder holder(directory.path());
    std::jthread release([&] {
        std::this_thread::sleep_for(100ms);
        holder.stop();
    });

    auto acquired = DataDirLock::acquire(directory.path(), 1000ms);
    release.join();
    expect(acquired.has_value(), "waiting acquisition should succeed after the owner exits");
}

void test_default_wait_is_five_seconds() {
    TempDirectory directory;
    LockHolder holder(directory.path());
    const auto started = std::chrono::steady_clock::now();
    expect_error(DataDirLock::acquire(directory.path()), ErrorCode::busy);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(elapsed >= 4900ms && elapsed < 7000ms, "default contention should wait about five seconds");
}

void test_invalid_arguments_and_filesystem_errors() {
    TempDirectory directory;
    expect_error(DataDirLock::acquire({}, 0ms), ErrorCode::invalid_argument);
    expect_error(DataDirLock::acquire(directory.path(), -1ms), ErrorCode::invalid_argument);
    auto invalid_path = directory.path().native();
    invalid_path.push_back('\0');
    invalid_path += "suffix";
    expect_error(DataDirLock::acquire(invalid_path, 0ms), ErrorCode::invalid_argument);
    expect(!std::filesystem::exists(directory.path() / ".mailai.lock"),
        "invalid arguments must not create a lock file");

    expect_error(DataDirLock::acquire(directory.path() / "missing", 0ms), ErrorCode::storage_error);
    std::filesystem::create_directory(directory.path() / ".mailai.lock");
    expect_error(DataDirLock::acquire(directory.path(), 0ms), ErrorCode::storage_error);
    std::filesystem::remove(directory.path() / ".mailai.lock");
    expect(DataDirLock::acquire(directory.path(), std::chrono::milliseconds::max()).has_value(),
        "a large timeout should not overflow before the first lock attempt");
}

void test_read_only_database_access_does_not_take_directory_lock() {
    TempDirectory directory;
    const auto path = directory.path() / "cache.sqlite3";
    expect(SqliteStore::open(path, OpenMode::read_write_create).has_value(), "cache should initialize");
    LockHolder holder(directory.path());

    auto store = SqliteStore::open(path, OpenMode::read_only);
    expect(store.has_value() && store->list({}).has_value(),
        "holding the directory lock must not block existing read-only cache access");
}
}  // namespace

int main() {
    try {
        test_scope_moves_and_file_preservation();
        test_move_assignment_transfers_ownership();
        test_process_contention_timeout_and_exit_recovery();
        test_wait_succeeds_after_another_process_releases();
        test_default_wait_is_five_seconds();
        test_invalid_arguments_and_filesystem_errors();
        test_read_only_database_access_does_not_take_directory_lock();
        std::cout << "data_dir_lock tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "mailai/store.h"
#include "test_support.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

using namespace mailai;

class TempDirectory {
public:
    TempDirectory()
    {
        auto pattern = (std::filesystem::temp_directory_path()
                        / "mailai-store-test-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error("could not create temporary test directory");
        }
        path_ = created;
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void test_create_database_and_reopen_after_scope_exit()
{
    TempDirectory directory;
    const auto path = directory.path() / "邮件缓存.sqlite3";

    {
        const auto result = SqliteStore::open(path, OpenMode::read_write_create);
        expect(result.has_value(),
               "read-write-create mode should open a new database");
        expect(std::filesystem::is_regular_file(path),
               "opening a new database should create a file at the requested path");
    }

    expect(std::filesystem::is_regular_file(path),
           "destroying the store should preserve the database file");

    const auto reopened = SqliteStore::open(path, OpenMode::read_only);
    expect(reopened.has_value(),
           "the database file should remain accessible in read-only mode");
}

void test_read_only_missing_database_does_not_create_file()
{
    TempDirectory directory;
    const auto path = directory.path() / "missing.sqlite3";

    const auto result = SqliteStore::open(path, OpenMode::read_only);
    expect(!result.has_value(),
           "read-only mode should reject a missing database");
    expect(result.error().code == ErrorCode::storage_error,
           "a missing database should report a storage error");
    expect(!result.error().message.empty(),
           "a database open error should include a diagnostic");
    expect(!std::filesystem::exists(path),
           "read-only mode should not create a missing database file");
}

void test_missing_parent_returns_storage_error()
{
    TempDirectory directory;
    const auto path = directory.path() / "missing-parent" / "mailai.sqlite3";

    const auto result = SqliteStore::open(path, OpenMode::read_write_create);
    expect(!result.has_value(),
           "opening a database below a missing parent should fail");
    expect(result.error().code == ErrorCode::storage_error,
           "an invalid database location should report a storage error");
    expect(!result.error().message.empty(),
           "an invalid database location should include a diagnostic");
    expect(!std::filesystem::exists(path.parent_path()),
           "opening a database should not create its parent directories");
}

} // namespace

int main()
{
    try {
        test_create_database_and_reopen_after_scope_exit();
        test_read_only_missing_database_does_not_create_file();
        test_missing_parent_returns_storage_error();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

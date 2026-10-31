#include "mailai/store.h"
#include "test_support.h"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

using namespace mailai;

class TempDirectory {
  public:
    TempDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "mailai-store-test-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error("could not create temporary test directory");
        }
        path_ = created;
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void test_create_database_and_reopen_after_scope_exit() {
    TempDirectory directory;
    const auto path = directory.path() / "邮件缓存.sqlite3";

    {
        const auto result = SqliteStore::open(path, OpenMode::read_write_create);
        expect(result.has_value(), "read-write-create mode should open a new database");
        expect(
            std::filesystem::is_regular_file(path),
            "opening a new database should create a file at the requested path"
        );
    }

    expect(
        std::filesystem::is_regular_file(path),
        "destroying the store should preserve the database file"
    );

    const auto reopened = SqliteStore::open(path, OpenMode::read_only);
    expect(reopened.has_value(), "the database file should remain accessible in read-only mode");
}

void test_read_only_missing_database_does_not_create_file() {
    TempDirectory directory;
    const auto path = directory.path() / "missing.sqlite3";

    const auto result = SqliteStore::open(path, OpenMode::read_only);
    expect(!result.has_value(), "read-only mode should reject a missing database");
    expect(
        result.error().code == ErrorCode::storage_error,
        "a missing database should report a storage error"
    );
    expect(!result.error().message.empty(), "a database open error should include a diagnostic");
    expect(
        !std::filesystem::exists(path),
        "read-only mode should not create a missing database file"
    );
}

void test_missing_parent_returns_storage_error() {
    TempDirectory directory;
    const auto path = directory.path() / "missing-parent" / "mailai.sqlite3";

    const auto result = SqliteStore::open(path, OpenMode::read_write_create);
    expect(!result.has_value(), "opening a database below a missing parent should fail");
    expect(
        result.error().code == ErrorCode::storage_error,
        "an invalid database location should report a storage error"
    );
    expect(
        !result.error().message.empty(),
        "an invalid database location should include a diagnostic"
    );
    expect(
        !std::filesystem::exists(path.parent_path()),
        "opening a database should not create its parent directories"
    );
}

void test_write_open_creates_accounts_table() {
    TempDirectory directory;
    const auto path = directory.path() / "accounts.sqlite3";
    {
        const auto result = SqliteStore::open(path, OpenMode::read_write_create);
        expect(result.has_value(), "opening a new mail store should succeed");
    }

    sqlite3* raw_connection = nullptr;
    const int open_rc =
        sqlite3_open_v2(path.c_str(), &raw_connection, SQLITE_OPEN_READONLY, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> connection{
        raw_connection,
        &sqlite3_close_v2
    };
    expect(open_rc == SQLITE_OK, "the created database should be readable independently");

    sqlite3_stmt* raw_statement = nullptr;
    const int prepare_rc = sqlite3_prepare_v2(
        connection.get(),
        "SELECT id, display_name, address, imap_host, imap_port, imap_username, "
        "credential_env, tls_mode FROM accounts;",
        -1,
        &raw_statement,
        nullptr
    );
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{
        raw_statement,
        &sqlite3_finalize
    };
    expect(
        prepare_rc == SQLITE_OK,
        "opening a new store should create account and IMAP configuration fields"
    );
    expect(
        sqlite3_step(statement.get()) == SQLITE_DONE,
        "a new accounts table should contain no sample accounts"
    );

    const auto reopened = SqliteStore::open(path, OpenMode::read_write_create);
    expect(reopened.has_value(), "initializing an existing accounts table should succeed");
}

void test_corrupt_database_is_preserved_when_initialization_fails() {
    TempDirectory directory;
    const auto path = directory.path() / "corrupt.sqlite3";
    const std::string original = "This file is not an SQLite database.\n";
    {
        std::ofstream output(path, std::ios::binary);
        output << original;
        output.close();
        expect(output.good(), "the corrupt database fixture should be written successfully");
    }

    const auto result = SqliteStore::open(path, OpenMode::read_write_create);
    expect(!result.has_value(), "a corrupt database should fail store initialization");
    expect(
        result.error().code == ErrorCode::storage_error,
        "a database initialization failure should report a storage error"
    );
    expect(
        !result.error().message.empty(),
        "a database initialization failure should include a diagnostic"
    );

    std::ifstream input(path, std::ios::binary);
    expect(input.is_open(), "initialization failure should preserve the existing file");
    const std::string remaining{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}
    };
    expect(
        remaining == original,
        "initialization failure should preserve the original database bytes"
    );
}

void test_account_save_update_and_identity_protection() {
    TempDirectory directory;
    const auto path = directory.path() / "account-save.sqlite3";
    {
        auto opened = SqliteStore::open(path, OpenMode::read_write_create);
        expect(opened.has_value(), "opening a new store should succeed");

        Account account{"school", "课程邮箱", "student@school.example"};
        ImapSettings imap;
        imap.host = "imap.school.example";
        imap.port = 993;
        imap.username = "student";
        imap.credential_env = "MAILAI_IMAP_PASSWORD";

        expect(
            opened->ensure_account(account, imap).has_value(),
            "saving an account should succeed"
        );
        expect(
            opened->ensure_account(account, imap).has_value(),
            "saving the same account again should succeed"
        );
        account.display_name = "Student's 学校邮箱";
        expect(
            opened->ensure_account(account, imap).has_value(),
            "updating an account should succeed"
        );

        for (int change = 0; change < 3; ++change) {
            auto other = imap;
            if (change == 0)
                other.host = "other.example";
            if (change == 1)
                other.port = 143;
            if (change == 2)
                other.username = "another-user";
            const auto result = opened->ensure_account(account, other);
            expect(!result.has_value(), "changing an existing account identity should fail");
            expect(!result.error().message.empty(), "identity mismatch should explain the error");
        }
    }

    sqlite3* raw_connection = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &raw_connection, SQLITE_OPEN_READONLY, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> connection{
        raw_connection,
        &sqlite3_close_v2
    };
    expect(rc == SQLITE_OK, "saved database should reopen");

    sqlite3_stmt* raw_statement = nullptr;
    const int prepare_rc = sqlite3_prepare_v2(
        connection.get(),
        "SELECT display_name, address, imap_host, imap_port, imap_username, credential_env, "
        "tls_mode "
        "FROM accounts WHERE id = 'school';",
        -1,
        &raw_statement,
        nullptr
    );
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{
        raw_statement,
        &sqlite3_finalize
    };
    expect(prepare_rc == SQLITE_OK, "saved account should be queryable");
    expect(sqlite3_step(statement.get()) == SQLITE_ROW, "saved account should exist after reopen");
    const auto text_column = [&](int column) {
        const auto* value = sqlite3_column_text(statement.get(), column);
        return value ? std::string{reinterpret_cast<const char*>(value)} : std::string{};
    };
    expect(
        text_column(0) == "Student's 学校邮箱",
        "updated display name should be preserved exactly"
    );
    expect(text_column(1) == "student@school.example", "account address should be preserved");
    expect(
        text_column(2) == "imap.school.example",
        "host should remain unchanged after rejected changes"
    );
    expect(
        sqlite3_column_type(statement.get(), 3) == SQLITE_INTEGER,
        "IMAP port should be stored as an integer"
    );
    expect(
        sqlite3_column_int(statement.get(), 3) == 993,
        "port should remain unchanged after rejected changes"
    );
    expect(text_column(4) == "student", "username should remain unchanged after rejected changes");
    expect(
        text_column(5) == "MAILAI_IMAP_PASSWORD",
        "only the credential variable name should be saved"
    );
    expect(text_column(6) == "implicit", "TLS mode should be preserved");
    expect(
        sqlite3_step(statement.get()) == SQLITE_DONE,
        "repeated saves should not create duplicate accounts"
    );
}

} // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[]{
        {"create_and_reopen", test_create_database_and_reopen_after_scope_exit},
        {"read_only_missing", test_read_only_missing_database_does_not_create_file},
        {"missing_parent", test_missing_parent_returns_storage_error},
        {"accounts_table", test_write_open_creates_accounts_table},
        {"corrupt_database", test_corrupt_database_is_preserved_when_initialization_fails},
        {"account_save", test_account_save_update_and_identity_protection}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

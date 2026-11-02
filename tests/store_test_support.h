#pragma once

#include <sqlite3.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "test_support.h"

namespace store_test {
class TempDirectory {
public:
    TempDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "mailai-store-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (!created)
            throw std::runtime_error("could not create test directory");
        path_ = created;
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
class Database {
public:
    explicit Database(const std::filesystem::path& path,
                      int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
        db_.reset(raw);
        expect(rc == SQLITE_OK, "test connection should open");
    }
    sqlite3* get() const {
        return db_.get();
    }
    Statement prepare(const char* sql) const {
        sqlite3_stmt* raw = nullptr;
        const int rc = sqlite3_prepare_v2(get(), sql, -1, &raw, nullptr);
        Statement statement{raw, &sqlite3_finalize};
        if (rc != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(get()));
        return statement;
    }
    void execute(const std::string& sql) const {
        if (sqlite3_exec(get(), sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(get()));
    }
    std::int64_t integer(const char* sql) const {
        auto statement = prepare(sql);
        expect(sqlite3_step(statement.get()) == SQLITE_ROW, "scalar query should return a row");
        return sqlite3_column_int64(statement.get(), 0);
    }
    std::string text(const char* sql) const {
        auto statement = prepare(sql);
        expect(sqlite3_step(statement.get()) == SQLITE_ROW, "text query should return a row");
        return column(statement.get(), 0);
    }
    std::vector<std::vector<std::string>> rows(const char* sql) const {
        auto statement = prepare(sql);
        std::vector<std::vector<std::string>> result;
        int rc;
        while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
            std::vector<std::string> row;
            for (int c = 0; c < sqlite3_column_count(statement.get()); ++c)
                row.push_back(column(statement.get(), c));
            result.push_back(std::move(row));
        }
        expect(rc == SQLITE_DONE, "row query should finish");
        return result;
    }
    auto catalog() const {
        return rows("SELECT type,name,tbl_name,sql FROM sqlite_schema ORDER BY type,name");
    }

private:
    static std::string column(sqlite3_stmt* statement, int c) {
        const auto* data = sqlite3_column_text(statement, c);
        return data ? std::string{reinterpret_cast<const char*>(data),
                                  static_cast<std::size_t>(sqlite3_column_bytes(statement, c))}
                    : "<NULL>";
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> db_{nullptr, &sqlite3_close_v2};
};

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    expect(input.good(), "fixture should be readable");
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
inline std::string legacy_sql() {
    return read_file(MAILAI_LEGACY_FIXTURE);
}
inline void insert_legacy_account(const Database& db) {
    db.execute(
        "INSERT INTO accounts VALUES ('school', 'Student''s 学校邮箱', 'student@example.invalid', "
        "'imap.example.invalid', 993, 'student', 'TEST_PASSWORD_REF', 'implicit')");
}
inline std::string replace_once(std::string source, std::string_view before,
                                std::string_view after) {
    const auto position = source.find(before);
    expect(position != std::string::npos, "schema mutation should match its fixture");
    source.replace(position, before.size(), after);
    return source;
}
}  // namespace store_test

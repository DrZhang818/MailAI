#pragma once

#include "mailai/config.h"
#include "mailai/model.h"

#include <filesystem>

struct sqlite3;

namespace mailai {

enum class OpenMode { read_only, read_write_create };

class SqliteStore {
  public:
    static Result<SqliteStore> open(const std::filesystem::path& path, OpenMode mode);

    ~SqliteStore();

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    SqliteStore(SqliteStore&& other) noexcept;
    SqliteStore& operator=(SqliteStore&& other) noexcept;

    Status ensure_account(const Account& account, const ImapSettings& imap);

  private:
    explicit SqliteStore(sqlite3* db) noexcept;

    sqlite3* db_{nullptr};
};

} // namespace mailai
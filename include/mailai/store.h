#pragma once

#include <filesystem>
#include <vector>

#include "mailai/config.h"
#include "mailai/inbox.h"
#include "mailai/model.h"

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

    Result<std::vector<InboxItem>> list(const InboxQuery& query);

    Result<Mail> find(MailId id);

private:
    explicit SqliteStore(sqlite3* db) noexcept;

    sqlite3* db_{nullptr};
};

}  // namespace mailai
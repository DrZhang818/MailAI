#include "mailai/store.h"

#include <sqlite3.h>
#include <utility>

namespace mailai {

SqliteStore::SqliteStore(sqlite3* db) noexcept : db_(db) {}

SqliteStore::~SqliteStore() {
    sqlite3_close_v2(db_);
}

SqliteStore::SqliteStore(SqliteStore&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

SqliteStore& SqliteStore::operator=(SqliteStore&& other) noexcept {
    if (this != &other) {
        sqlite3_close_v2(db_);
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

Result<SqliteStore> SqliteStore::open(const std::filesystem::path& path, OpenMode mode) {
    int flags = SQLITE_OPEN_READONLY;

    if (mode == OpenMode::read_write_create) {
        flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    }

    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);

    SqliteStore store(db);

    if (rc != SQLITE_OK) {
        return std::unexpected(
            Error{
                ErrorCode::storage_error,
                db ? sqlite3_errmsg(db) : "Failed to allocate SQLite connection",
                std::nullopt,
                std::nullopt,
                "storage.open"
            }
        );
    }

    return store;
};

} // namespace mailai
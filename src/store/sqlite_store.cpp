#include "mailai/store.h"

#include <sqlite3.h>

namespace mailai {

Result<SqliteStore> SqliteStore::open(const std::filesystem::path& path, OpenMode mode) {
    int flags = SQLITE_OPEN_READONLY;

    if(mode == OpenMode::read_write_create) {
        flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    }

    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);

    SqliteStore store(db);

    if(rc != SQLITE_OK) {
        return std::unexpected(Error{
            ErrorCode::storage_error,
            db ? sqlite3_errmsg(db) : "Failed to allocate SQLite connection",
            std::nullopt,
            std::nullopt,
            "storage.open"
        });
    }

    return store;
};

} // namsspace mailai
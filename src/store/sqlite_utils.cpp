#include "sqlite_utils.h"

#include <limits>
#include <string>
#include <utility>

namespace mailai::store_detail {
namespace {
Error diagnostic(std::string_view message, std::string_view stage) {
    return {ErrorCode::storage_error, std::string{message}, std::nullopt, std::nullopt,
            std::string{stage}};
}
}  // namespace

Error sqlite_error(sqlite3* db, int rc, std::string_view stage) {
    const int primary = rc & 0xff;
    const auto code = primary == SQLITE_BUSY || primary == SQLITE_LOCKED ? ErrorCode::busy
                                                                         : ErrorCode::storage_error;
    return {code, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc), std::nullopt, std::nullopt,
            std::string{stage}};
}

Result<Statement> prepare(sqlite3* db, std::string_view sql, std::string_view stage) {
    if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::unexpected(diagnostic("SQL statement exceeds SQLite size limit", stage));
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw, nullptr);
    Statement statement{raw, &sqlite3_finalize};
    if (rc != SQLITE_OK)
        return std::unexpected(sqlite_error(db, rc, stage));
    if (!statement)
        return std::unexpected(diagnostic("SQL statement is empty", stage));
    return statement;
}

Status execute(sqlite3* db, const char* sql, std::string_view stage) {
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK)
        return std::unexpected(sqlite_error(db, rc, stage));
    return {};
}

Result<std::int64_t> query_integer(sqlite3* db, const char* sql, std::string_view stage) {
    auto statement = prepare(db, sql, stage);
    if (!statement)
        return std::unexpected(statement.error());
    int rc = sqlite3_step(statement->get());
    if (rc != SQLITE_ROW) {
        if (rc != SQLITE_DONE)
            return std::unexpected(sqlite_error(db, rc, stage));
        return std::unexpected(diagnostic("Expected one integer result", stage));
    }
    if (sqlite3_column_count(statement->get()) != 1 ||
        sqlite3_column_type(statement->get(), 0) != SQLITE_INTEGER)
        return std::unexpected(diagnostic("Expected one integer result", stage));
    const auto value = sqlite3_column_int64(statement->get(), 0);
    rc = sqlite3_step(statement->get());
    if (rc != SQLITE_DONE) {
        if (rc != SQLITE_ROW)
            return std::unexpected(sqlite_error(db, rc, stage));
        return std::unexpected(diagnostic("Expected exactly one result row", stage));
    }
    return value;
}

Status configure_connection(sqlite3* db) {
    const int extended_rc = sqlite3_extended_result_codes(db, 1);
    if (extended_rc != SQLITE_OK)
        return std::unexpected(sqlite_error(db, extended_rc, "storage.configure"));
    const int timeout_rc = sqlite3_busy_timeout(db, 1000);
    if (timeout_rc != SQLITE_OK)
        return std::unexpected(sqlite_error(db, timeout_rc, "storage.configure"));
    auto enabled = execute(db, "PRAGMA foreign_keys=ON", "storage.configure");
    if (!enabled)
        return enabled;
    auto value = query_integer(db, "PRAGMA foreign_keys", "storage.configure");
    if (!value)
        return std::unexpected(value.error());
    if (*value != 1)
        return std::unexpected(
            diagnostic("SQLite foreign keys could not be enabled", "storage.configure"));
    return {};
}

Status configure_wal(sqlite3* db) {
    const char* filename = sqlite3_db_filename(db, "main");
    if (!filename || *filename == '\0')
        return {};  // In-memory or temporary database.
    auto statement = prepare(db, "PRAGMA main.journal_mode=WAL", "storage.configure");
    if (!statement)
        return std::unexpected(statement.error());
    const int rc = sqlite3_step(statement->get());
    if (rc != SQLITE_ROW) {
        if (rc != SQLITE_DONE)
            return std::unexpected(sqlite_error(db, rc, "storage.configure"));
        return std::unexpected(
            diagnostic("SQLite did not return a journal mode", "storage.configure"));
    }
    const auto* value = sqlite3_column_text(statement->get(), 0);
    if (!value || std::string_view{reinterpret_cast<const char*>(value)} != "wal")
        return std::unexpected(
            diagnostic("SQLite WAL mode could not be enabled", "storage.configure"));
    const int finish_rc = sqlite3_step(statement->get());
    if (finish_rc != SQLITE_DONE)
        return std::unexpected(sqlite_error(db, finish_rc, "storage.configure"));
    return {};
}

Result<Transaction> Transaction::begin(sqlite3* db, TransactionMode mode) {
    auto status = execute(db, mode == TransactionMode::write ? "BEGIN IMMEDIATE" : "BEGIN",
                          "storage.transaction");
    if (!status)
        return std::unexpected(status.error());
    return Transaction{db};
}
Transaction::Transaction(Transaction&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}
Transaction::~Transaction() {
    if (db_)
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
}
Status Transaction::commit() {
    auto status = execute(db_, "COMMIT", "storage.transaction");
    if (status)
        db_ = nullptr;
    return status;
}
}  // namespace mailai::store_detail

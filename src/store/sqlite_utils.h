#pragma once

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "mailai/model.h"

namespace mailai::store_detail {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Error sqlite_error(sqlite3* db, int rc, std::string_view stage);
Result<Statement> prepare(sqlite3* db, std::string_view sql, std::string_view stage);
Status execute(sqlite3* db, const char* sql, std::string_view stage);
Result<std::int64_t> query_integer(sqlite3* db, const char* sql, std::string_view stage);
Status configure_connection(sqlite3* db);
Status configure_wal(sqlite3* db);

enum class TransactionMode { read, write };
class Transaction {
public:
    static Result<Transaction> begin(sqlite3* db, TransactionMode mode);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Status commit();

private:
    explicit Transaction(sqlite3* db) noexcept : db_(db) {}
    sqlite3* db_;
};

struct BlobView {
    std::string_view bytes;
};

using SqlValue = std::variant<std::nullptr_t, std::int64_t, std::string_view, BlobView>;

// Borrows the connection; shares SQLite mechanics and diagnostic context for one operation.
class SqlContext {
public:
    SqlContext(sqlite3* db, std::string_view stage, std::optional<AccountId> account_id = {});

    std::string_view stage() const noexcept {
        return stage_;
    }
    std::unexpected<Error> fail(ErrorCode code, std::string_view message, std::optional<MailId> mail_id = {}) const;
    std::unexpected<Error> fail(Error error) const;

    Result<Statement> prepare(std::string_view sql, std::initializer_list<SqlValue> values = {}) const;
    Status bind(const Statement& statement, int first_index, std::initializer_list<SqlValue> values) const;
    // A successful false value means the query has reached SQLITE_DONE.
    Result<bool> next(const Statement& statement) const;
    Status finish(const Statement& statement) const;
    // Execute a write, then reset it for reuse without clearing its bound values.
    Status execute(const Statement& statement, std::initializer_list<SqlValue> values = {}) const;
    Status execute(std::string_view sql, std::initializer_list<SqlValue> values = {}) const;
    Result<Transaction> begin(TransactionMode mode) const;
    Status commit(Transaction& transaction) const;

private:
    sqlite3* db_;
    std::string stage_;
    std::optional<AccountId> account_id_;
};

std::string column_text(sqlite3_stmt* statement, int column);
}  // namespace mailai::store_detail

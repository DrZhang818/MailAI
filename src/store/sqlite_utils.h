#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string_view>

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
}  // namespace mailai::store_detail

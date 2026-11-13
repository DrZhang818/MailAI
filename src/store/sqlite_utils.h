#pragma once

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "mailai/model.h"

namespace mailai::store_detail {

Error sqlite_error(sqlite3* db, int rc, std::string_view stage);
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
    explicit Transaction(sqlite3* db) noexcept;
    static Status _execute(sqlite3* db, const char* sql);

    sqlite3* db_;
};

struct BlobView {
    std::string_view bytes;
};

using SqlValue = std::variant<std::nullptr_t, std::int64_t, std::string_view, BlobView>;

class SqlContext;

// A view of the current row. Decoders copy values before the next SQLite step.
class SqlRow {
public:
    std::unexpected<Error> fail(std::string_view message) const;

    template <class... Values>
    Status read(Values&... values) const;

private:
    friend class SqlContext;
    SqlRow(sqlite3_stmt* statement, const SqlContext& context);

    Status column(int index, std::int64_t& value) const;
    Status column(int index, std::uint64_t& value) const;
    Status column(int index, std::uint32_t& value) const;
    Status column(int index, bool& value) const;
    Status column(int index, std::string& value) const;

    template <class T>
    Status column(int index, std::optional<T>& value) const;

    sqlite3_stmt* statement_;
    const SqlContext& context_;
};

template <class T>
Result<T> decode_scalar(const SqlRow& row);

template <class Decoder>
using DecodedValue = typename std::invoke_result_t<Decoder, const SqlRow&>::value_type;

// Borrows the connection; shares SQLite mechanics and diagnostic context for one operation.
class SqlContext {
public:
    SqlContext(
        sqlite3* db,
        std::string_view stage,
        std::optional<AccountId> account_id = {},
        std::optional<MailId> mail_id = {}
    );

    std::unexpected<Error> fail(ErrorCode code, std::string_view message, std::optional<MailId> mail_id = {}) const;
    std::unexpected<Error> fail(Error error) const;

    Status execute(std::string_view sql, std::initializer_list<SqlValue> values = {}) const;

    // Prepare once, bind common parameters once, then fill the remaining parameters per row.
    // Each row must supply the complete suffix. The caller owns the surrounding transaction.
    template <std::ranges::input_range Rows>
    Status execute_many(
        std::string_view sql, std::initializer_list<SqlValue> common, Rows&& rows
    ) const;

    Result<Transaction> begin(TransactionMode mode) const;
    Status commit(Transaction& transaction) const;
    int changes() const;
    Result<std::int64_t> query_integer(std::string_view sql) const;

    // Zero rows is an empty optional. More than one row is a storage error.
    template <class Decoder>
    Result<std::optional<DecodedValue<Decoder>>> query_one(
        std::string_view sql, std::initializer_list<SqlValue> values, Decoder decode
    ) const;

    // The visitor handles one row at a time. The count includes rows it chooses to skip.
    template <class Visitor>
    Result<std::size_t> scan(
        std::string_view sql, std::initializer_list<SqlValue> values, Visitor visit
    ) const;

    template <class Decoder>
    Result<std::vector<DecodedValue<Decoder>>> query_all(
        std::string_view sql, std::initializer_list<SqlValue> values, Decoder decode
    ) const;

private:
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

    Result<Statement> _prepare(std::string_view sql, std::initializer_list<SqlValue> values) const;
    Status _bind(const Statement& statement, int first_index, std::span<const SqlValue> values) const;
    // A successful false value means the query has reached SQLITE_DONE.
    Result<bool> _next(const Statement& statement) const;
    Status _finish(const Statement& statement) const;
    Status _execute(const Statement& statement) const;

    sqlite3* db_;
    std::string stage_;
    std::optional<AccountId> account_id_;
    std::optional<MailId> mail_id_;
};

}  // namespace mailai::store_detail

#include "sqlite_utils.hpp"

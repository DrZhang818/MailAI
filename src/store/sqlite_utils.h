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

// 当前数据行的视图；解码函数在 SQLite 推进到下一行之前复制字段值。
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

// 借用数据库连接，在单次操作中统一 SQLite 操作和错误上下文。
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

    // 单条语句须绑定所有参数位置，使用 nullptr 绑定 SQL NULL。
    Status execute(std::string_view sql, std::initializer_list<SqlValue> values = {}) const;

    // 只准备一次语句并绑定公共参数，逐行绑定剩余参数。
    // 每行必须提供完整的剩余参数，外层事务由调用方管理。
    template <std::ranges::input_range Rows>
    Status execute_many(
        std::string_view sql, std::initializer_list<SqlValue> common, Rows&& rows
    ) const;

    Result<Transaction> begin(TransactionMode mode) const;
    Status commit(Transaction& transaction) const;
    int changes() const;
    Result<std::int64_t> query_integer(std::string_view sql) const;

    // 没有结果时返回空 optional，多于一行时返回存储错误。
    template <class Decoder>
    Result<std::optional<DecodedValue<Decoder>>> query_one(
        std::string_view sql, std::initializer_list<SqlValue> values, Decoder decode
    ) const;

    // 访问函数逐行处理结果，计数包含主动跳过的行。
    template <class Visitor>
    Result<std::size_t> scan(
        std::string_view sql, std::initializer_list<SqlValue> values, Visitor visit
    ) const;

    template <class Decoder>
    Result<std::vector<DecodedValue<Decoder>>> query_all(
        std::string_view sql, std::initializer_list<SqlValue> values, Decoder decode
    ) const;

private:
    enum class BindMode { complete, prefix };
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

    Result<Statement> _prepare(
        std::string_view sql,
        std::initializer_list<SqlValue> values,
        BindMode bind_mode = BindMode::complete
    ) const;
    Status _bind(const Statement& statement, int first_index, std::span<const SqlValue> values) const;
    // 成功返回 false 表示查询已到达 SQLITE_DONE。
    Result<bool> _next(const Statement& statement) const;
    Status _finish(const Statement& statement) const;
    Status _execute(const Statement& statement) const;

    sqlite3* db_;
    std::string stage_;
    std::optional<AccountId> account_id_;
    std::optional<MailId> mail_id_;
};

}  // 命名空间 mailai::store_detail

#include "sqlite_utils.hpp"

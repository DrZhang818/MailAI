#include "sqlite_utils.h"

#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace mailai::store_detail {

Error sqlite_error(sqlite3* db, int rc, std::string_view stage) {
    const int primary = rc & 0xff;
    const auto code   = primary == SQLITE_BUSY || primary == SQLITE_LOCKED ? ErrorCode::busy
                                                                           : ErrorCode::storage_error;
    return {
        code,
        db ? sqlite3_errmsg(db) : sqlite3_errstr(rc),
        std::nullopt,
        std::nullopt,
        std::string{stage}
    };
}

Result<SqlContext::Statement> SqlContext::_prepare(std::string_view sql, std::initializer_list<SqlValue> values) const {
    if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return fail(ErrorCode::storage_error, "SQL statement exceeds SQLite size limit");

    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &raw, nullptr);
    Statement statement{raw, &sqlite3_finalize};
    if (rc != SQLITE_OK) return fail(sqlite_error(db_, rc, stage_));
    if (!statement) return fail(ErrorCode::storage_error, "SQL statement is empty");

    auto bound = _bind(statement, 1, {values.begin(), values.size()});
    if (!bound) return std::unexpected(bound.error());

    return statement;
}

Status Transaction::_execute(sqlite3* db, const char* sql) {
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) return std::unexpected(sqlite_error(db, rc, "storage.transaction"));

    return {};
}

Result<std::int64_t> SqlContext::query_integer(std::string_view sql) const {
    auto value = query_one(sql, {}, decode_scalar<std::int64_t>);
    if (!value) return std::unexpected(value.error());
    if (!*value) return fail(ErrorCode::storage_error, "Expected one integer result");

    return **value;
}

Status configure_connection(sqlite3* db) {
    const SqlContext context{db, "storage.configure"};
    const int extended_rc = sqlite3_extended_result_codes(db, 1);
    if (extended_rc != SQLITE_OK) return std::unexpected(sqlite_error(db, extended_rc, "storage.configure"));

    const int timeout_rc = sqlite3_busy_timeout(db, 1000);
    if (timeout_rc != SQLITE_OK) return std::unexpected(sqlite_error(db, timeout_rc, "storage.configure"));

    auto enabled = context.execute("PRAGMA foreign_keys=ON");
    if (!enabled) return enabled;

    auto value = context.query_integer("PRAGMA foreign_keys");
    if (!value) return std::unexpected(value.error());
    if (*value != 1)
        return context.fail(ErrorCode::storage_error, "SQLite foreign keys could not be enabled");

    return {};
}

Status configure_wal(sqlite3* db) {
    const char* filename = sqlite3_db_filename(db, "main");
    if (!filename || *filename == '\0')
        return {};  // In-memory or temporary database.

    const SqlContext context{db, "storage.configure"};
    auto value = context.query_one("PRAGMA main.journal_mode=WAL", {}, decode_scalar<std::string>);
    if (!value) return std::unexpected(value.error());
    if (!*value) return context.fail(ErrorCode::storage_error, "SQLite did not return a journal mode");
    if (**value != "wal") return context.fail(ErrorCode::storage_error, "SQLite WAL mode could not be enabled");

    return {};
}

Transaction::Transaction(sqlite3* db) noexcept : db_(db) {}

Result<Transaction> Transaction::begin(sqlite3* db, TransactionMode mode) {
    auto status = _execute(db, mode == TransactionMode::write ? "BEGIN IMMEDIATE" : "BEGIN");
    if (!status) return std::unexpected(status.error());

    return Transaction{db};
}

Transaction::Transaction(Transaction&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Transaction::~Transaction() {
    if (db_)
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
}

Status Transaction::commit() {
    auto status = _execute(db_, "COMMIT");
    if (status)
        db_ = nullptr;
    return status;
}

SqlRow::SqlRow(sqlite3_stmt* statement, const SqlContext& context)
    : statement_(statement), context_(context) {}

std::unexpected<Error> SqlRow::fail(std::string_view message) const {
    return context_.fail(ErrorCode::storage_error, message);
}

Status SqlRow::column(int index, std::int64_t& value) const {
    if (sqlite3_column_type(statement_, index) != SQLITE_INTEGER)
        return fail("Expected an INTEGER SQL value at column " + std::to_string(index));

    value = sqlite3_column_int64(statement_, index);
    return {};
}

Status SqlRow::column(int index, std::uint64_t& value) const {
    std::int64_t integer{};
    auto decoded = column(index, integer);
    if (!decoded) return decoded;
    if (integer < 0) return fail("Expected a nonnegative SQL integer at column " + std::to_string(index));

    value = static_cast<std::uint64_t>(integer);
    return {};
}

Status SqlRow::column(int index, std::uint32_t& value) const {
    std::uint64_t integer{};
    auto decoded = column(index, integer);
    if (!decoded) return decoded;
    if (integer > std::numeric_limits<std::uint32_t>::max())
        return fail("SQL integer exceeds the UID range at column " + std::to_string(index));

    value = static_cast<std::uint32_t>(integer);
    return {};
}

Status SqlRow::column(int index, bool& value) const {
    std::int64_t integer{};
    auto decoded = column(index, integer);
    if (!decoded) return decoded;
    if (integer != 0 && integer != 1) return fail("Expected a SQL boolean at column " + std::to_string(index));

    value = integer != 0;
    return {};
}

Status SqlRow::column(int index, std::string& value) const {
    if (sqlite3_column_type(statement_, index) != SQLITE_TEXT)
        return fail("Expected a TEXT SQL value at column " + std::to_string(index));

    const auto* text = sqlite3_column_text(statement_, index);
    if (!text) return fail("Could not read a TEXT SQL value");

    value.assign(reinterpret_cast<const char*>(text), sqlite3_column_bytes(statement_, index));
    return {};
}

SqlContext::SqlContext(
    sqlite3* db,
    std::string_view stage,
    std::optional<AccountId> account_id,
    std::optional<MailId> mail_id
) : db_(db), stage_(stage), account_id_(std::move(account_id)), mail_id_(mail_id) {}

std::unexpected<Error> SqlContext::fail(ErrorCode code, std::string_view message, std::optional<MailId> mail_id) const {
    return std::unexpected(Error{code, std::string{message}, account_id_, mail_id ? mail_id : mail_id_, stage_});
}

std::unexpected<Error> SqlContext::fail(Error error) const {
    if (account_id_)
        error.account_id = account_id_;
    if (mail_id_)
        error.mail_id = mail_id_;
    return std::unexpected(std::move(error));
}

Status SqlContext::_bind(const Statement& statement, int first_index, std::span<const SqlValue> values) const {
    int index = first_index;
    for (const auto& value : values) {
        const int rc = std::visit(
            [&](const auto& parameter) {
                using T = std::decay_t<decltype(parameter)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) {
                    return sqlite3_bind_null(statement.get(), index);
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return sqlite3_bind_int64(statement.get(), index, parameter);
                } else if constexpr (std::is_same_v<T, std::string_view>) {
                    return sqlite3_bind_text64(
                        statement.get(),
                        index,
                        parameter.empty() ? "" : parameter.data(),
                        parameter.size(),
                        SQLITE_TRANSIENT,
                        SQLITE_UTF8
                    );
                } else {
                    return sqlite3_bind_blob64(
                        statement.get(),
                        index,
                        parameter.bytes.empty() ? "" : parameter.bytes.data(),
                        parameter.bytes.size(),
                        SQLITE_TRANSIENT
                    );
                }
            },
            value);
        if (rc != SQLITE_OK) return fail(sqlite_error(db_, rc, stage_));
        ++index;
    }
    return {};
}

Result<bool> SqlContext::_next(const Statement& statement) const {
    const int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    return fail(sqlite_error(db_, rc, stage_));
}

Status SqlContext::_finish(const Statement& statement) const {
    const int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_ROW) return fail(ErrorCode::storage_error, "Unexpected additional SQL result row");
    if (rc != SQLITE_DONE) return fail(sqlite_error(db_, rc, stage_));
    return {};
}

Status SqlContext::_execute(const Statement& statement) const {
    auto done = _finish(statement);
    if (!done) return done;

    const int rc = sqlite3_reset(statement.get());
    if (rc != SQLITE_OK) return fail(sqlite_error(db_, rc, stage_));

    return {};
}

Status SqlContext::execute(std::string_view sql, std::initializer_list<SqlValue> values) const {
    auto statement = _prepare(sql, values);
    if (!statement) return std::unexpected(statement.error());

    return _execute(*statement);
}

Result<Transaction> SqlContext::begin(TransactionMode mode) const {
    auto transaction = Transaction::begin(db_, mode);
    if (!transaction) return fail(transaction.error());

    return transaction;
}

Status SqlContext::commit(Transaction& transaction) const {
    auto committed = transaction.commit();
    if (!committed) return fail(committed.error());

    return {};
}

int SqlContext::changes() const {
    return sqlite3_changes(db_);
}

}  // namespace mailai::store_detail

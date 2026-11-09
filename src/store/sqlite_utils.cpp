#include "sqlite_utils.h"

#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace mailai::store_detail {
namespace {
Error diagnostic(std::string_view message, std::string_view stage) {
    return {
        ErrorCode::storage_error,
        std::string{message},
        std::nullopt,
        std::nullopt,
        std::string{stage}
    };
}
}  // namespace

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

Result<Statement> prepare(sqlite3* db, std::string_view sql, std::string_view stage) {
    if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::unexpected(diagnostic("SQL statement exceeds SQLite size limit", stage));

    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw, nullptr);
    Statement statement{raw, &sqlite3_finalize};
    if (rc != SQLITE_OK) return std::unexpected(sqlite_error(db, rc, stage));
    if (!statement) return std::unexpected(diagnostic("SQL statement is empty", stage));

    return statement;
}

Status execute(sqlite3* db, const char* sql, std::string_view stage) {
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) return std::unexpected(sqlite_error(db, rc, stage));

    return {};
}

Result<std::int64_t> query_integer(sqlite3* db, const char* sql, std::string_view stage) {
    auto statement = prepare(db, sql, stage);
    if (!statement) return std::unexpected(statement.error());

    int rc = sqlite3_step(statement->get());
    if (rc != SQLITE_ROW) {
        if (rc != SQLITE_DONE) return std::unexpected(sqlite_error(db, rc, stage));
        return std::unexpected(diagnostic("Expected one integer result", stage));
    }

    if (sqlite3_column_count(statement->get()) != 1 ||
        sqlite3_column_type(statement->get(), 0) != SQLITE_INTEGER)
        return std::unexpected(diagnostic("Expected one integer result", stage));

    const auto value = sqlite3_column_int64(statement->get(), 0);
    rc = sqlite3_step(statement->get());
    if (rc != SQLITE_DONE) {
        if (rc != SQLITE_ROW) return std::unexpected(sqlite_error(db, rc, stage));
        return std::unexpected(diagnostic("Expected exactly one result row", stage));
    }

    return value;
}

Status configure_connection(sqlite3* db) {
    const int extended_rc = sqlite3_extended_result_codes(db, 1);
    if (extended_rc != SQLITE_OK) return std::unexpected(sqlite_error(db, extended_rc, "storage.configure"));

    const int timeout_rc = sqlite3_busy_timeout(db, 1000);
    if (timeout_rc != SQLITE_OK) return std::unexpected(sqlite_error(db, timeout_rc, "storage.configure"));

    auto enabled = execute(db, "PRAGMA foreign_keys=ON", "storage.configure");
    if (!enabled) return enabled;

    auto value = query_integer(db, "PRAGMA foreign_keys", "storage.configure");
    if (!value) return std::unexpected(value.error());
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
    if (!statement) return std::unexpected(statement.error());

    const int rc = sqlite3_step(statement->get());
    if (rc != SQLITE_ROW) {
        if (rc != SQLITE_DONE) return std::unexpected(sqlite_error(db, rc, "storage.configure"));
        return std::unexpected(
            diagnostic("SQLite did not return a journal mode", "storage.configure"));
    }

    const auto* value = sqlite3_column_text(statement->get(), 0);
    if (!value || std::string_view{reinterpret_cast<const char*>(value)} != "wal")
        return std::unexpected(
            diagnostic("SQLite WAL mode could not be enabled", "storage.configure"));

    const int finish_rc = sqlite3_step(statement->get());
    if (finish_rc != SQLITE_DONE) return std::unexpected(sqlite_error(db, finish_rc, "storage.configure"));

    return {};
}

Result<Transaction> Transaction::begin(sqlite3* db, TransactionMode mode) {
    auto status = execute(
        db, mode == TransactionMode::write ? "BEGIN IMMEDIATE" : "BEGIN", "storage.transaction");
    if (!status) return std::unexpected(status.error());

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

SqlContext::SqlContext(sqlite3* db, std::string_view stage, std::optional<AccountId> account_id)
    : db_(db), stage_(stage), account_id_(std::move(account_id)) {}

std::unexpected<Error> SqlContext::fail(ErrorCode code, std::string_view message, std::optional<MailId> mail_id) const {
    return std::unexpected(Error{code, std::string{message}, account_id_, mail_id, stage_});
}

std::unexpected<Error> SqlContext::fail(Error error) const {
    if (account_id_)
        error.account_id = account_id_;
    return std::unexpected(std::move(error));
}

Result<Statement> SqlContext::prepare(std::string_view sql, std::initializer_list<SqlValue> values) const {
    auto statement = store_detail::prepare(db_, sql, stage_);
    if (!statement) return fail(statement.error());

    auto bound = bind(*statement, 1, values);
    if (!bound) return std::unexpected(bound.error());

    return statement;
}

Status SqlContext::bind(const Statement& statement, int first_index, std::initializer_list<SqlValue> values) const {
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

Result<bool> SqlContext::next(const Statement& statement) const {
    const int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    return fail(sqlite_error(db_, rc, stage_));
}

Status SqlContext::finish(const Statement& statement) const {
    const int rc = sqlite3_step(statement.get());
    if (rc != SQLITE_DONE) return fail(sqlite_error(db_, rc, stage_));
    return {};
}

Status SqlContext::execute(const Statement& statement, std::initializer_list<SqlValue> values) const {
    auto bound = bind(statement, 1, values);
    if (!bound) return bound;

    auto done = finish(statement);
    if (!done) return done;

    const int rc = sqlite3_reset(statement.get());
    if (rc != SQLITE_OK) return fail(sqlite_error(db_, rc, stage_));

    return {};
}

Status SqlContext::execute(std::string_view sql, std::initializer_list<SqlValue> values) const {
    auto statement = prepare(sql, values);
    if (!statement) return std::unexpected(statement.error());

    return execute(*statement);
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

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? std::string{reinterpret_cast<const char*>(value),
                               static_cast<std::size_t>(sqlite3_column_bytes(statement, column))}
                 : std::string{};
}
}  // namespace mailai::store_detail

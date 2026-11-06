#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "mailai/store.h"
#include "sqlite_utils.h"

namespace mailai {

Result<SyncState> SqliteStore::state(const AccountId& account_id, std::string_view folder) {
    constexpr std::string_view stage = "storage.state";

    const auto fail = [&](ErrorCode code, const char* message) -> Result<SyncState> {
        return std::unexpected(Error{code, message, account_id, std::nullopt, std::string{stage}});
    };

    const auto sql_failure = [&](int rc) -> Result<SyncState> {
        auto error = store_detail::sqlite_error(db_, rc, stage);
        error.account_id = account_id;
        return std::unexpected(std::move(error));
    };

    if (account_id.empty() || folder.empty())
        return fail(ErrorCode::invalid_argument, "Account and folder must not be empty");

    constexpr const char* sql = R"sql(
        SELECT s.uid_validity, COALESCE(s.last_discovered_uid, 0),
               s.last_attempt_at, s.last_success_at
        FROM accounts AS a
        LEFT JOIN sync_state AS s ON s.account_id = a.id AND s.folder = ?2
        WHERE a.id = ?1
    )sql";

    auto prepared = store_detail::prepare(db_, sql, stage);
    if (!prepared) {
        auto error = prepared.error();
        error.account_id = account_id;
        return std::unexpected(std::move(error));
    }
    auto statement = std::move(*prepared);

    const std::pair<int, std::string_view> fields[]{{1, account_id}, {2, folder}};
    for (const auto& [index, value] : fields) {
        const int rc = sqlite3_bind_text64(
            statement.get(), index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        if (rc != SQLITE_OK)
            return sql_failure(rc);
    }

    int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_DONE)
        return fail(ErrorCode::unknown_account, "Unknown account");
    if (rc != SQLITE_ROW)
        return sql_failure(rc);

    const auto optional_integer = [&](int column) {
        const int type = sqlite3_column_type(statement.get(), column);
        return type == SQLITE_NULL || type == SQLITE_INTEGER;
    };

    if (!optional_integer(0) || sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER ||
        !optional_integer(2) || !optional_integer(3))
        return fail(ErrorCode::storage_error, "Invalid stored sync state");

    const bool has_validity = sqlite3_column_type(statement.get(), 0) != SQLITE_NULL;
    const auto validity = sqlite3_column_int64(statement.get(), 0);
    const auto cursor = sqlite3_column_int64(statement.get(), 1);
    constexpr std::int64_t max_uid = std::numeric_limits<std::uint32_t>::max();
    if ((has_validity && (validity < 1 || validity > max_uid)) || cursor < 0 || cursor > max_uid ||
        (!has_validity && cursor != 0))
        return fail(ErrorCode::storage_error, "Invalid stored sync state");

    SyncState result;
    if (has_validity)
        result.uid_validity = static_cast<std::uint32_t>(validity);
    result.last_discovered_uid = static_cast<std::uint32_t>(cursor);
    if (sqlite3_column_type(statement.get(), 2) != SQLITE_NULL)
        result.last_attempt_at = sqlite3_column_int64(statement.get(), 2);
    if (sqlite3_column_type(statement.get(), 3) != SQLITE_NULL)
        result.last_success_at = sqlite3_column_int64(statement.get(), 3);

    rc = sqlite3_step(statement.get());
    if (rc != SQLITE_DONE)
        return sql_failure(rc);

    return result;
}

}  // namespace mailai

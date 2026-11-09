#include <algorithm>
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

Result<std::vector<RemoteKey>> SqliteStore::pending(
    const AccountId& account_id, std::string_view folder) {
    using PendingResult = Result<std::vector<RemoteKey>>;
    constexpr std::string_view stage = "storage.pending";

    const auto fail = [&](ErrorCode code, const char* message) -> PendingResult {
        return std::unexpected(Error{code, message, account_id, std::nullopt, std::string{stage}});
    };

    const auto sql_failure = [&](int rc) -> PendingResult {
        auto error = store_detail::sqlite_error(db_, rc, stage);
        error.account_id = account_id;
        return std::unexpected(std::move(error));
    };

    if (account_id.empty() || folder.empty())
        return fail(ErrorCode::invalid_argument, "Account and folder must not be empty");

    constexpr const char* sql = R"sql(    
        SELECT p.uid_validity, p.uid
        FROM accounts AS a
        LEFT JOIN sync_state AS s
            ON s.account_id = a.id AND s.folder = ?2
        LEFT JOIN sync_pending AS p
            ON p.account_id = s.account_id
            AND p.folder = s.folder
            AND p.uid_validity = s.uid_validity
            AND p.retryable = 1
        WHERE a.id = ?1
        ORDER BY p.uid ASC
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

    std::vector<RemoteKey> result;
    constexpr std::int64_t max_uid = std::numeric_limits<std::uint32_t>::max();

    do {
        const int validity_type = sqlite3_column_type(statement.get(), 0);
        const int uid_type = sqlite3_column_type(statement.get(), 1);

        if (validity_type != SQLITE_NULL || uid_type != SQLITE_NULL) {
            if (validity_type != SQLITE_INTEGER || uid_type != SQLITE_INTEGER)
                return fail(ErrorCode::storage_error, "Invalid stored pending job");

            const auto validity = sqlite3_column_int64(statement.get(), 0);
            const auto uid = sqlite3_column_int64(statement.get(), 1);
            if (validity < 1 || validity > max_uid || uid < 1 || uid > max_uid)
                return fail(ErrorCode::storage_error, "Invalid stored pending job");

            result.push_back(
                {account_id,
                    std::string{folder},
                    static_cast<std::uint32_t>(validity),
                    static_cast<std::uint32_t>(uid)});
        }

        rc = sqlite3_step(statement.get());
    } while (rc == SQLITE_ROW);

    if (rc != SQLITE_DONE)
        return sql_failure(rc);

    return result;
}

Status SqliteStore::schedule(const AccountId& account_id,
    std::string_view folder,
    const MailboxSnapshot& snapshot,
    std::size_t initial_limit) {
    constexpr std::string_view stage = "storage.schedule";

    const auto fail = [&](ErrorCode code, const char* message) -> Status {
        return std::unexpected(Error{code, message, account_id, std::nullopt, std::string{stage}});
    };
    const auto with_account = [&](Error error) -> Status {
        error.account_id = account_id;
        return std::unexpected(std::move(error));
    };
    const auto sql_failure = [&](int rc) -> Status {
        return with_account(store_detail::sqlite_error(db_, rc, stage));
    };

    if (account_id.empty() || folder.empty() || snapshot.uid_validity == 0 || initial_limit == 0)
        return fail(ErrorCode::invalid_argument, "Invalid discovery settings");
    if (std::find(snapshot.uids.begin(), snapshot.uids.end(), 0) != snapshot.uids.end())
        return fail(ErrorCode::invalid_argument, "Messages UIDs must not be zero");

    auto uids = snapshot.uids;
    std::sort(uids.begin(), uids.end());
    uids.erase(std::unique(uids.begin(), uids.end()), uids.end());

    auto transaction = store_detail::Transaction::begin(db_, store_detail::TransactionMode::write);
    if (!transaction)
        return with_account(transaction.error());

    auto current = state(account_id, folder);
    if (!current) {
        auto error = current.error();
        error.stage = std::string{stage};
        return with_account(std::move(error));
    }

    const bool new_range =
        !current->uid_validity || *current->uid_validity != snapshot.uid_validity;

    const bool range_changed =
        current->uid_validity && *current->uid_validity != snapshot.uid_validity;
    auto first = uids.begin();
    if (new_range)
        first = uids.end() - std::min(uids.size(), initial_limit);
    else
        first = std::upper_bound(uids.begin(), uids.end(), current->last_discovered_uid);

    std::uint32_t discovered = uids.empty() ? 0 : uids.back();
    if (!new_range)
        discovered = std::max(discovered, current->last_discovered_uid);

    const auto prepare_for_folder = [&](const char* sql) -> Result<store_detail::Statement> {
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
            if (rc != SQLITE_OK) {
                auto error = store_detail::sqlite_error(db_, rc, stage);
                error.account_id = account_id;
                return std::unexpected(std::move(error));
            }
        }
        return statement;
    };

    if (range_changed) {
        constexpr const char* cleanup_sql[]{
            R"sql(
                UPDATE messages SET historical = 1
                WHERE account_id = ?1 AND folder = ?2 AND uid_validity <> ?3 
            )sql",
            R"sql(
                DELETE FROM sync_pending
                WHERE account_id = ?1 AND folder = ?2 AND uid_validity <> ?3
            )sql"
        };
        for (const auto* sql : cleanup_sql) {
            auto prepared = prepare_for_folder(sql);
            if (!prepared)
                return std::unexpected(prepared.error());
            auto statement = std::move(*prepared);
            int rc = sqlite3_bind_int64(statement.get(), 3, snapshot.uid_validity);
            if (rc != SQLITE_OK)
                return sql_failure(rc);
            rc = sqlite3_step(statement.get());
            if (rc != SQLITE_DONE)
                return sql_failure(rc);
        }
    }

    constexpr const char* save_stage_sql = R"sql(
        INSERT INTO sync_state(account_id, folder, uid_validity, last_discovered_uid)
        VALUES (?1, ?2, ?3, ?4)
        ON CONFLICT(account_id, folder) DO UPDATE SET
            uid_validity = excluded.uid_validity,
            last_discovered_uid = excluded.last_discovered_uid
    )sql";
    auto prepared_state = prepare_for_folder(save_stage_sql);
    if (!prepared_state)
        return std::unexpected(prepared_state.error());
    auto state_statement = std::move(*prepared_state);
    const std::pair<int, uint32_t> state_values[]{{3, snapshot.uid_validity}, {4, discovered}};
    for (const auto& [index, value] : state_values) {
        const int rc = sqlite3_bind_int64(state_statement.get(), index, value);
        if (rc != SQLITE_OK)
            return sql_failure(rc);
    }
    int rc = sqlite3_step(state_statement.get());
    if (rc != SQLITE_DONE)
        return sql_failure(rc);

    constexpr const char* save_job_sql = R"sql(
        INSERT INTO sync_pending(account_id, folder, uid_validity, uid)
        SELECT ?1, ?2, ?3, ?4
        WHERE NOT EXISTS (
            SELECT 1 FROM messages
            WHERE account_id = ?1 AND folder = ?2 AND uid_validity = ?3
                AND uid = ?4 AND parse_state = 'parsed'
        )
        ON CONFLICT(account_id, folder, uid_validity, uid) DO NOTHING
    )sql";
    auto prepared_job = prepare_for_folder(save_job_sql);
    if (!prepared_job)
        return std::unexpected(prepared_job.error());
    auto job_statement = std::move(*prepared_job);
    rc = sqlite3_bind_int64(job_statement.get(), 3, snapshot.uid_validity);
    if (rc != SQLITE_OK)
        return sql_failure(rc);
    for (auto it = first; it != uids.end(); ++it) {
        rc = sqlite3_bind_int64(job_statement.get(), 4, *it);
        if (rc != SQLITE_OK)
            return sql_failure(rc);
        rc = sqlite3_step(job_statement.get());
        if (rc != SQLITE_DONE)
            return sql_failure(rc);
        rc = sqlite3_reset(job_statement.get());
        if (rc != SQLITE_OK)
            return sql_failure(rc);
    }

    auto committed = transaction->commit();
    if (!committed)
        return with_account(committed.error());

    return {};
}

}  // namespace mailai

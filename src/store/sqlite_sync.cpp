#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "mailai/store.h"
#include "sqlite_utils.h"

namespace mailai {
using store_detail::SqlContext;

Result<SyncState> SqliteStore::state(const AccountId& account_id, std::string_view folder) {
    const SqlContext context{db_, "storage.state", account_id};
    if (account_id.empty() || folder.empty()) {
        return context.fail(ErrorCode::invalid_argument, "Account and folder must not be empty");
    }

    constexpr const char* sql = R"sql(
        SELECT s.uid_validity, COALESCE(s.last_discovered_uid, 0),
               s.last_attempt_at, s.last_success_at
        FROM accounts AS a
        LEFT JOIN sync_state AS s ON s.account_id = a.id AND s.folder = ?2
        WHERE a.id = ?1
    )sql";

    auto statement = context.prepare(sql, {account_id, folder});
    if (!statement) return std::unexpected(statement.error());

    auto found = context.next(*statement);
    if (!found)  return std::unexpected(found.error());
    if (!*found) return context.fail(ErrorCode::unknown_account, "Unknown account");

    if (sqlite3_column_type(statement->get(), 1) != SQLITE_INTEGER) {
        return context.fail(ErrorCode::storage_error, "Invalid stored sync state");
    }
    for (const int column : {0, 2, 3}) {
        const int type = sqlite3_column_type(statement->get(), column);
        if (type != SQLITE_NULL && type != SQLITE_INTEGER) {
            return context.fail(ErrorCode::storage_error, "Invalid stored sync state");
        }
    }

    const bool has_validity = sqlite3_column_type(statement->get(), 0) != SQLITE_NULL;
    const auto validity     = sqlite3_column_int64(statement->get(), 0);
    const auto cursor       = sqlite3_column_int64(statement->get(), 1);
    constexpr std::int64_t max_uid = std::numeric_limits<std::uint32_t>::max();
    if ((has_validity && (validity < 1 || validity > max_uid)) || cursor < 0 || cursor > max_uid || (!has_validity && cursor != 0)) {
        return context.fail(ErrorCode::storage_error, "Invalid stored sync state");
    }

    SyncState result;
    if (has_validity) result.uid_validity = static_cast<std::uint32_t>(validity);
    result.last_discovered_uid = static_cast<std::uint32_t>(cursor);
    if (sqlite3_column_type(statement->get(), 2) != SQLITE_NULL) result.last_attempt_at = sqlite3_column_int64(statement->get(), 2);
    if (sqlite3_column_type(statement->get(), 3) != SQLITE_NULL) result.last_success_at = sqlite3_column_int64(statement->get(), 3);

    auto finished = context.finish(*statement);
    if (!finished) return std::unexpected(finished.error());

    return result;
}

Result<std::vector<RemoteKey>> SqliteStore::pending(const AccountId& account_id, std::string_view folder) {
    const SqlContext context{db_, "storage.pending", account_id};
    if (account_id.empty() || folder.empty()) {
        return context.fail(ErrorCode::invalid_argument, "Account and folder must not be empty");
    }

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

    auto statement = context.prepare(sql, {account_id, folder});
    if (!statement) return std::unexpected(statement.error());

    auto row = context.next(*statement);
    if (!row)   return std::unexpected(row.error());
    if (!*row)  return context.fail(ErrorCode::unknown_account, "Unknown account");

    std::vector<RemoteKey> result;

    constexpr std::int64_t max_uid = std::numeric_limits<std::uint32_t>::max();

    do {
        const int validity_type = sqlite3_column_type(statement->get(), 0);
        const int uid_type      = sqlite3_column_type(statement->get(), 1);

        if (validity_type != SQLITE_NULL || uid_type != SQLITE_NULL) {
            if (validity_type != SQLITE_INTEGER || uid_type != SQLITE_INTEGER) {
                return context.fail(ErrorCode::storage_error, "Invalid stored pending job");
            }

            const auto validity = sqlite3_column_int64(statement->get(), 0);
            const auto uid      = sqlite3_column_int64(statement->get(), 1);
            if (validity < 1 || validity > max_uid || uid < 1 || uid > max_uid) {
                return context.fail(ErrorCode::storage_error, "Invalid stored pending job");
            }

            result.emplace_back(
                account_id,
                std::string{folder},
                static_cast<std::uint32_t>(validity),
                static_cast<std::uint32_t>(uid)
            );
        }

        row = context.next(*statement);
        if (!row) return std::unexpected(row.error());
    } while (*row);

    return result;
}

Status SqliteStore::schedule(const AccountId& account_id, std::string_view folder, const MailboxSnapshot& snapshot, std::size_t initial_limit) {
    const SqlContext context{db_, "storage.schedule", account_id};
    if (account_id.empty() || folder.empty() || snapshot.uid_validity == 0 || initial_limit == 0) {
        return context.fail(ErrorCode::invalid_argument, "Invalid discovery settings");
    }
    if (std::find(snapshot.uids.begin(), snapshot.uids.end(), 0) != snapshot.uids.end()) {
        return context.fail(ErrorCode::invalid_argument, "Messages UIDs must not be zero");
    }

    auto uids = snapshot.uids;
    std::sort(uids.begin(), uids.end());
    uids.erase(std::unique(uids.begin(), uids.end()), uids.end());

    auto transaction = context.begin(store_detail::TransactionMode::write);
    if (!transaction) return std::unexpected(transaction.error());

    auto current = state(account_id, folder);
    if (!current) {
        auto error = current.error();
        error.stage = std::string{context.stage()};
        return context.fail(std::move(error));
    }

    const bool new_range        = !current->uid_validity || *current->uid_validity != snapshot.uid_validity;
    const bool range_changed    = current->uid_validity && *current->uid_validity != snapshot.uid_validity;

    auto first = uids.begin();
    if (new_range)  first = uids.end() - std::min(uids.size(), initial_limit);
    else            first = std::upper_bound(uids.begin(), uids.end(), current->last_discovered_uid);

    std::uint32_t discovered = uids.empty() ? 0 : uids.back();
    if (!new_range) discovered = std::max(discovered, current->last_discovered_uid);

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
        for (const auto* cleanup : cleanup_sql) {
            auto cleaned = context.execute(cleanup, {account_id, folder, snapshot.uid_validity});
            if (!cleaned) return cleaned;
        }
    }

    constexpr const char* save_state_sql = R"sql(
        INSERT INTO sync_state(account_id, folder, uid_validity, last_discovered_uid)
        VALUES (?1, ?2, ?3, ?4)
        ON CONFLICT(account_id, folder) DO UPDATE SET
            uid_validity = excluded.uid_validity,
            last_discovered_uid = excluded.last_discovered_uid
    )sql";
    auto saved = context.execute(save_state_sql, {account_id, folder, snapshot.uid_validity, discovered});
    if (!saved) return saved;

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
    auto job = context.prepare(save_job_sql, {account_id, folder, snapshot.uid_validity});
    if (!job) return std::unexpected(job.error());
    for (auto it = first; it != uids.end(); ++it) {
        auto bound = context.bind(*job, 4, {*it});
        if (!bound) return bound;

        auto added = context.execute(*job);
        if (!added) return added;
    }

    return context.commit(*transaction);
}

}  // namespace mailai

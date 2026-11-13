#include <algorithm>
#include <array>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "mailai/store.h"
#include "sqlite_rows.h"
#include "sqlite_sync.h"
#include "sqlite_utils.h"

namespace mailai {
using store_detail::SqlContext;
using store_detail::SqlRow;
using store_detail::SqlValue;

namespace {
struct PendingCollector {
    const AccountId& account_id;
    std::string_view folder;
    std::vector<RemoteKey>& jobs;

    Status operator()(const SqlRow& row) const {
        std::optional<std::uint32_t> validity;
        std::optional<std::uint32_t> uid;
        auto decoded = row.read(validity, uid);
        if (!decoded) return decoded;
        if (!validity && !uid) return {};
        if (!validity || !uid || *validity == 0 || *uid == 0)
            return row.fail("Invalid stored pending job");

        jobs.emplace_back(account_id, std::string{folder}, *validity, *uid);
        return {};
    }
};

Status retire_previous_range(const SqlContext& context, const AccountId& account_id, std::string_view folder, std::uint32_t validity) {
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
        auto cleaned = context.execute(cleanup, {account_id, folder, validity});
        if (!cleaned) return cleaned;
    }
    return {};
}

Status save_discovery(
    const SqlContext& context,
    const AccountId& account_id,
    std::string_view folder,
    std::uint32_t validity,
    std::uint32_t discovered
) {
    constexpr const char* save_state_sql = R"sql(
        INSERT INTO sync_state(account_id, folder, uid_validity, last_discovered_uid)
        VALUES (?1, ?2, ?3, ?4)
        ON CONFLICT(account_id, folder) DO UPDATE SET
            uid_validity = excluded.uid_validity,
            last_discovered_uid = excluded.last_discovered_uid
    )sql";
    return context.execute(save_state_sql, {account_id, folder, validity, discovered});
}

Status register_pending(
    const SqlContext& context,
    const AccountId& account_id,
    std::string_view folder,
    std::uint32_t validity,
    std::span<const std::uint32_t> uids
) {
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
    auto rows = uids | std::views::transform([](std::uint32_t uid) {
        return std::array<SqlValue, 1>{uid};
    });
    return context.execute_many(save_job_sql, {account_id, folder, validity}, rows);
}
}  // namespace

namespace store_detail {
Result<SyncState> load_sync_state(
    const SqlContext& context, const AccountId& account_id, std::string_view folder
) {
    constexpr const char* sql = R"sql(
        SELECT s.uid_validity, COALESCE(s.last_discovered_uid, 0),
               s.last_attempt_at, s.last_success_at
        FROM accounts AS a
        LEFT JOIN sync_state AS s ON s.account_id = a.id AND s.folder = ?2
        WHERE a.id = ?1
    )sql";

    auto found = context.query_one(sql, {account_id, folder}, decode_sync_state);
    if (!found)  return std::unexpected(found.error());
    if (!*found) return context.fail(ErrorCode::unknown_account, "Unknown account");

    return std::move(**found);
}
}  // namespace store_detail

Result<SyncState> SqliteStore::state(const AccountId& account_id, std::string_view folder) {
    const SqlContext context{db_, "storage.state", account_id};
    if (account_id.empty() || folder.empty()) {
        return context.fail(ErrorCode::invalid_argument, "Account and folder must not be empty");
    }
    return store_detail::load_sync_state(context, account_id, folder);
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

    std::vector<RemoteKey> result;
    auto rows = context.scan(sql, {account_id, folder}, PendingCollector{account_id, folder, result});
    if (!rows)  return std::unexpected(rows.error());
    if (!*rows) return context.fail(ErrorCode::unknown_account, "Unknown account");

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

    auto current = store_detail::load_sync_state(context, account_id, folder);
    if (!current) return std::unexpected(current.error());

    const bool new_range        = !current->uid_validity || *current->uid_validity != snapshot.uid_validity;
    const bool range_changed    = current->uid_validity && *current->uid_validity != snapshot.uid_validity;

    auto first = uids.begin();
    if (new_range)  first = uids.end() - std::min(uids.size(), initial_limit);
    else            first = std::upper_bound(uids.begin(), uids.end(), current->last_discovered_uid);

    std::uint32_t discovered = uids.empty() ? 0 : uids.back();
    if (!new_range) discovered = std::max(discovered, current->last_discovered_uid);

    if (range_changed) {
        auto retired = retire_previous_range(context, account_id, folder, snapshot.uid_validity);
        if (!retired) return retired;
    }

    auto saved = save_discovery(context, account_id, folder, snapshot.uid_validity, discovered);
    if (!saved) return saved;

    auto registered = register_pending(context, account_id, folder, snapshot.uid_validity, {first, uids.end()});
    if (!registered) return registered;

    return context.commit(*transaction);
}

}  // namespace mailai

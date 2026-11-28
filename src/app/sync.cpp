#include "mailai/sync.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>

#include "mailai/error_message.h"
#include "mailai/imap.h"
#include "mailai/mime.h"
#include "mailai/store.h"
#include "sync_source.h"

namespace mailai {
namespace {

namespace errors = error_message::sync;

constexpr std::string_view inbox_folder = "INBOX";
constexpr std::size_t initial_limit = 20;

void add_failure(SyncSummary& summary, const RemoteKey& key, Error error) {
    error.account_id = key.account_id;
    summary.failures.push_back({key, std::move(error)});
    ++summary.failed;
}

Status record_failure(SqliteStore& store, const RemoteKey& key, Error error, SyncSummary& summary) {
    auto recorded = store.fail(key, error);
    if (!recorded)
        return recorded;
    add_failure(summary, key, std::move(error));
    return {};
}

Status process_mail(SqliteStore& store,
    const RuntimeConfig& config,
    const sync_detail::MailSource& source,
    const RemoteKey& key,
    SyncSummary& summary) {
    auto downloaded = source.fetch(config, key);
    if (!downloaded)
        return record_failure(store, key, std::move(downloaded.error()), summary);

    const auto& remote = downloaded->key;
    if (remote.account_id != key.account_id || remote.folder != key.folder ||
        remote.uid_validity != key.uid_validity || remote.uid != key.uid) {
        return record_failure(store,
            key,
            {ErrorCode::protocol_error,
                errors::unexpected_remote_key,
                key.account_id,
                std::nullopt,
                errors::fetch_stage},
            summary);
    }

    auto parsed = parse_message(downloaded->raw);
    auto saved = store.complete(*downloaded, parsed);
    if (!saved)
        return record_failure(store, key, std::move(saved.error()), summary);

    if (parsed) {
        ++summary.success;
    } else {
        // complete 已经在同一事务中保存原文、解析错误和待办状态。
        parsed.error().mail_id = *saved;
        add_failure(summary, key, std::move(parsed.error()));
    }
    return {};
}

Result<SyncSummary> run_sync_round(SqliteStore& store,
    const RuntimeConfig& config,
    const sync_detail::MailSource& source,
    std::size_t history_limit) {
    const auto& account_id = config.account.id;
    auto previous = store.state(account_id, inbox_folder);
    if (!previous)
        return std::unexpected(previous.error());

    auto snapshot = source.read_snapshot(config);
    if (!snapshot)
        return std::unexpected(snapshot.error());

    SyncSummary summary;
    summary.reindexed = previous->uid_validity && *previous->uid_validity != snapshot->uid_validity;
    std::size_t previous_pending = 0;
    if (previous->uid_validity && !summary.reindexed) {
        const auto retries = store.pending(account_id, inbox_folder);
        if (!retries)
            return std::unexpected(retries.error());
        previous_pending = retries->size();
    }

    auto scheduled =
        store.schedule(account_id, inbox_folder, *snapshot, initial_limit, history_limit);
    if (!scheduled)
        return std::unexpected(scheduled.error());

    auto jobs = store.pending(account_id, inbox_folder);
    if (!jobs)
        return std::unexpected(jobs.error());

    // 同一 UIDVALIDITY 下原有待办保持不变，新增数量同时包含增量任务和历史补收任务。
    summary.discovered = jobs->size() - previous_pending;

    // 此时发现进度和待办已经提交，网络请求期间不持有写事务。
    for (const auto& key : *jobs) {
        auto processed = process_mail(store, config, source, key, summary);
        if (!processed)
            return std::unexpected(processed.error());
    }
    return summary;
}

}  // namespace

namespace sync_detail {

Result<SyncSummary> sync_inbox(SqliteStore& store,
    const RuntimeConfig& config,
    const MailSource& source,
    std::int64_t attempted_at,
    std::size_t history_limit) {
    if (!source.read_snapshot || !source.fetch) {
        return std::unexpected(Error{
            ErrorCode::invalid_argument,
            errors::invalid_mail_source,
            config.account.id,
            std::nullopt,
            errors::source_stage
        });
    }

    auto account = store.ensure_account(config.account, config.imap);
    if (!account)
        return std::unexpected(account.error());

    auto outcome = run_sync_round(store, config, source, history_limit);
    auto recorded = store.record_outcome(config.account.id, inbox_folder, outcome, attempted_at);
    if (!recorded)
        return std::unexpected(recorded.error());
    return outcome;
}

}  // namespace sync_detail

Result<SyncSummary> sync_inbox(
    SqliteStore& store, const RuntimeConfig& config, std::size_t history_limit) {
    const auto attempted_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    ImapSession session{config};
    const sync_detail::MailSource source{
        [&session](const RuntimeConfig&) { return session.read_mailbox_snapshot(); },
        [&session](const RuntimeConfig&, const RemoteKey& key) { return session.fetch_mail(key); }
    };
    return sync_detail::sync_inbox(store, config, source, attempted_at, history_limit);
}

}  // namespace mailai

#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

#include "mailai/config.h"
#include "mailai/inbox.h"
#include "mailai/mime.h"
#include "mailai/model.h"
#include "mailai/sync_types.h"

struct sqlite3;

namespace mailai {

enum class OpenMode { read_only, read_write_create };

class SqliteStore {
public:
    static Result<SqliteStore> open(const std::filesystem::path& path, OpenMode mode);

    ~SqliteStore();

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    SqliteStore(SqliteStore&& other) noexcept;
    SqliteStore& operator=(SqliteStore&& other) noexcept;

    Status ensure_account(const Account& account, const ImapSettings& imap);

    Result<std::vector<InboxItem>> list(const InboxQuery& query);

    Result<Mail> find(MailId id);

    Result<SyncState> state(const AccountId& account_id, std::string_view folder);

    Result<std::vector<RemoteKey>> pending(const AccountId& account_id, std::string_view folder);

    Status schedule(const AccountId& account_id,
        std::string_view folder,
        const MailboxSnapshot& snapshot,
        std::size_t initial_limit);

    Result<MailId> complete(const FetchedMail& mail, const Result<ParsedMail>& parsed);

    Status fail(const RemoteKey& key, const Error& error);

    Status record_outcome(const AccountId& account_id,
        std::string_view folder,
        const Result<SyncSummary>& outcome,
        std::int64_t attempted_at);

private:
    explicit SqliteStore(sqlite3* db) noexcept;

    sqlite3* db_{nullptr};
};

}  // namespace mailai

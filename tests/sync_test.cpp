#include "mailai/sync.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/sync_source.h"
#include "mailai/store.h"
#include "store_test_support.h"

namespace {

using namespace mailai;
using store_test::Database;
using store_test::TempDirectory;
using sync_detail::MailSource;

RuntimeConfig config() {
    RuntimeConfig result;
    result.account = {"school", "学校邮箱", "student@example.test"};
    result.imap.host = "imap.example.test";
    result.imap.username = "student";
    result.imap.credential_env = "TEST_PASSWORD_REF";
    return result;
}

const std::string& sample() {
    static const auto raw = store_test::read_file("multipart_mixed_text_attachment.eml");
    return raw;
}

Error network_failure(ErrorCode code = ErrorCode::network_error) {
    return {code, "Synthetic mail source failure", "school", std::nullopt, "imap.fetch"};
}

struct Interrupted {};

struct ScriptedSource {
    MailboxSnapshot snapshot{77, {}};
    std::optional<Error> snapshot_error;
    std::map<std::uint32_t, Error> failures;
    std::map<std::uint32_t, std::string> raw_overrides;
    std::vector<std::uint32_t> requested;
    std::size_t snapshots{};
    std::function<void(const RemoteKey&)> before_fetch;

    MailSource source() {
        return {
            [&](const RuntimeConfig& settings) -> Result<MailboxSnapshot> {
                expect(settings.account.id == "school", "snapshot uses the configured account");
                ++snapshots;
                if (snapshot_error)
                    return std::unexpected(*snapshot_error);
                return snapshot;
            },
            [&](const RuntimeConfig& settings, const RemoteKey& key) -> Result<FetchedMail> {
                expect(key.account_id == settings.account.id && key.folder == "INBOX" &&
                           key.uid_validity == snapshot.uid_validity,
                    "download must use the active account and mailbox identity");
                requested.push_back(key.uid);
                if (before_fetch)
                    before_fetch(key);
                if (const auto error = failures.find(key.uid); error != failures.end()) {
                    return std::unexpected(error->second);
                }
                const auto raw = raw_overrides.find(key.uid);
                return FetchedMail{key, -10, raw == raw_overrides.end() ? sample() : raw->second};
            }
        };
    }
};

SqliteStore open_store(const std::filesystem::path& path) {
    auto store = SqliteStore::open(path, OpenMode::read_write_create);
    expect(store.has_value(), "temporary store should open");
    return std::move(*store);
}

void expect_summary(const Result<SyncSummary>& result,
    std::size_t discovered,
    std::size_t success,
    std::size_t failed,
    bool reindexed = false) {
    expect(result.has_value(), "sync round should return a summary");
    expect(result->discovered == discovered && result->success == success &&
               result->failed == failed && result->reindexed == reindexed &&
               result->failures.size() == failed,
        "sync summary should distinguish newly discovered work, successes and failures");
}

void expect_pending(SqliteStore& store, const std::vector<std::uint32_t>& expected) {
    const auto jobs = store.pending("school", "INBOX");
    expect(jobs.has_value(), "pending tasks should be readable");
    std::vector<std::uint32_t> uids;
    for (const auto& key : *jobs) uids.push_back(key.uid);
    expect(uids == expected, "unexpected remaining pending tasks");
}

void test_initial_limit_and_incremental_recovery() {
    TempDirectory directory;
    const auto path = directory.path() / "incremental.sqlite3";
    auto store = open_store(path);
    Database db(path);
    ScriptedSource mail;
    for (std::uint32_t uid = 25; uid > 0; --uid) mail.snapshot.uids.push_back(uid);
    mail.snapshot.uids.push_back(25);
    mail.failures.emplace(6, network_failure());
    const std::string invalid_raw =
        "Content-Type: text/plain; charset=x-mailai-unknown\r\n\r\nHello";
    mail.raw_overrides.emplace(7, invalid_raw);
    mail.failures.emplace(8, network_failure(ErrorCode::remote_gone));

    const auto first = sync_detail::sync_inbox(store, config(), mail.source(), 10);
    expect_summary(first, 20, 17, 3);
    expect(mail.requested.front() == 6 && mail.requested.back() == 25 &&
               mail.requested.size() == 20 && std::ranges::is_sorted(mail.requested),
        "initial sync should process the latest 20 unique UIDs in order");
    expect_pending(store, {6, 7});
    expect(db.integer("SELECT retryable FROM sync_pending WHERE uid=8") == 0,
        "confirmed remote disappearance stops retries");
    expect(first->failures[0].key.uid == 6 && first->failures[1].key.uid == 7 &&
               first->failures[1].error.code == ErrorCode::parse_error &&
               first->failures[1].error.account_id == "school" &&
               first->failures[1].error.mail_id &&
               first->failures[2].error.code == ErrorCode::remote_gone,
        "summary should identify each individual failure");
    const auto failed_id = *first->failures[1].error.mail_id;
    const auto failed = store.find(failed_id);
    expect(failed && failed->parse_state == ParseState::failed && !failed->parse_error.empty(),
        "parse failure should preserve a local record");
    expect(db.text("SELECT CAST(raw AS TEXT) FROM messages WHERE uid=7") == invalid_raw &&
               db.text("SELECT typeof(raw) FROM messages WHERE uid=7") == "blob",
        "parse failure should retain the original bytes");
    expect(db.text("SELECT last_error_code FROM sync_pending WHERE uid=6") == "network_error" &&
               db.text("SELECT last_error_code FROM sync_pending WHERE uid=7") == "parse_error",
        "failure reasons should be stored with retryable jobs");
    const auto first_state = store.state("school", "INBOX");
    expect(first_state && first_state->last_discovered_uid == 25 &&
               first_state->last_attempt_at == 10 && !first_state->last_success_at,
        "partial failure should advance discovery but not successful sync time");

    std::erase(mail.snapshot.uids, 8U);
    mail.snapshot.uids.insert(mail.snapshot.uids.end(), {27, 26, 27});
    mail.failures.clear();
    mail.raw_overrides.clear();
    mail.requested.clear();
    const auto second = sync_detail::sync_inbox(store, config(), mail.source(), 20);
    expect_summary(second, 2, 4, 0);
    expect(mail.requested == std::vector<std::uint32_t>{6, 7, 26, 27},
        "only failed tasks and newly discovered messages should be downloaded");
    expect_pending(store, {});
    const auto repaired = store.find(failed_id);
    expect(repaired && repaired->parse_state == ParseState::parsed &&
               repaired->parse_error.empty() && repaired->subject == "会议提醒" &&
               repaired->body.find("请在下午三点") != std::string::npos &&
               repaired->received_at_utc == -10 && repaired->attachments.size() == 1 &&
               repaired->attachments[0].name == "notes.txt",
        "MIME recovery preserves local ID and stores parsed content and attachment metadata");
    expect(
        db.integer("SELECT count(*) FROM messages") == 21, "one record per successful remote key");
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 30), 0, 0, 0);
    expect(mail.requested.empty(), "repeat sync must not download completed or retired work");
    const auto state = store.state("school", "INBOX");
    expect(state && state->last_discovered_uid == 27 && state->last_attempt_at == 30 &&
               state->last_success_at == 30,
        "a successful no-op sync should refresh both timestamps");
}

void test_history_backfill_keeps_incremental_sync_and_retries() {
    TempDirectory directory;
    const auto path = directory.path() / "history.sqlite3";
    auto store = open_store(path);
    Database db(path);
    ScriptedSource mail;
    for (std::uint32_t uid = 25; uid > 0; --uid) mail.snapshot.uids.push_back(uid);
    mail.snapshot.uids.push_back(5);
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 10), 20, 20, 0);
    const auto saved_id = db.integer("SELECT id FROM messages WHERE uid=6");

    mail.snapshot.uids.push_back(26);
    mail.failures.emplace(5, network_failure());
    mail.failures.emplace(4, network_failure(ErrorCode::remote_gone));
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 20, 2), 3, 1, 2);
    expect(mail.requested == std::vector<std::uint32_t>{4, 5, 26},
        "history should fetch the newest missing older mail in addition to new arrivals");
    expect_pending(store, {5});

    mail.failures.clear();
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 30, 2), 2, 3, 0);
    expect(mail.requested == std::vector<std::uint32_t>{2, 3, 5},
        "already pending or retired UIDs must not consume the history limit or be rescheduled");
    expect_pending(store, {});

    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 40), 0, 0, 0);
    expect(mail.requested.empty(), "ordinary sync must still leave unrequested history alone");

    const auto all = std::numeric_limits<std::size_t>::max();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 50, all), 1, 1, 0);
    expect(mail.requested == std::vector<std::uint32_t>{1}, "all should fetch remaining history");
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 60, all), 0, 0, 0);
    expect(mail.requested.empty() && db.integer("SELECT count(*) FROM messages") == 25 &&
               db.integer("SELECT id FROM messages WHERE uid=6") == saved_id,
        "repeating all must not download cached mail or change stable local IDs");
    const auto state = store.state("school", "INBOX");
    expect(state && state->last_discovered_uid == 26 && state->last_success_at == 60,
        "history backfill must not rewind incremental discovery");
}

void test_history_backfill_on_first_sync_and_reindex() {
    TempDirectory directory;
    const auto path = directory.path() / "history-reindex.sqlite3";
    auto store = open_store(path);
    Database db(path);
    ScriptedSource mail;
    for (std::uint32_t uid = 25; uid > 0; --uid) mail.snapshot.uids.push_back(uid);
    mail.failures.emplace(4, network_failure());
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 10, 2), 22, 21, 1);
    expect(
        mail.requested.size() == 22 && mail.requested.front() == 4 && mail.requested.back() == 25,
        "first history sync should extend the ordinary initial range by the requested count");
    expect_pending(store, {4});

    mail.snapshot.uid_validity = 78;
    mail.failures.clear();
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(
                       store, config(), mail.source(), 20, std::numeric_limits<std::size_t>::max()),
        25,
        25,
        0,
        true);
    expect(
        mail.requested.size() == 25 && mail.requested.front() == 1 && mail.requested.back() == 25,
        "all on a new UIDVALIDITY must not skip UIDs cached under the previous identity");
    expect_pending(store, {});
    expect(
        db.integer("SELECT count(*) FROM messages WHERE historical=1 AND uid_validity=77") == 21 &&
            db.integer("SELECT count(*) FROM messages WHERE historical=0 AND uid_validity=78") ==
                25 &&
            db.integer("SELECT count(*) FROM sync_pending WHERE uid_validity=77") == 0,
        "reindex must retain old mail history and retire old failures");
}

void test_uidvalidity_change_retires_old_range() {
    TempDirectory directory;
    const auto path = directory.path() / "reindex.sqlite3";
    auto store = open_store(path);
    Database db(path);
    ScriptedSource mail;
    mail.snapshot.uids = {101, 102, 103};
    mail.failures.emplace(102, network_failure());
    auto changed = network_failure(ErrorCode::remote_gone);
    changed.message = "Synthetic UIDVALIDITY change during download";
    mail.failures.emplace(103, changed);
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 10), 3, 1, 2);
    expect_pending(store, {102});
    const auto old_id = static_cast<MailId>(db.integer("SELECT id FROM messages WHERE uid=101"));
    mail.snapshot = {78, {101}};
    mail.failures.clear();
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 20), 1, 1, 0, true);
    expect(mail.requested == std::vector<std::uint32_t>{101}, "do not reuse the old pending UID");
    const auto old = store.find(old_id);
    expect(
        old && old->historical && old->uid_validity == 77, "prior mail should remain historical");
    expect(db.integer("SELECT count(*) FROM messages WHERE uid=101") == 2 &&
               db.integer("SELECT count(*) FROM sync_pending WHERE uid_validity=77") == 0,
        "new identity receives a separate local record and old jobs are retired");
    expect_pending(store, {});
}

void test_disappeared_mail_preserves_local_record() {
    TempDirectory directory;
    const auto path = directory.path() / "disappeared.sqlite3";
    auto store = open_store(path);
    Database db(path);
    ScriptedSource mail;
    mail.snapshot.uids = {101};
    mail.raw_overrides.emplace(101, "");
    const auto first = sync_detail::sync_inbox(store, config(), mail.source(), 10);
    expect_summary(first, 1, 0, 1);
    expect(first->failures[0].error.mail_id.has_value(), "parse failure should have a local ID");
    const auto id = *first->failures[0].error.mail_id;
    expect_pending(store, {101});

    mail.snapshot.uids.clear();
    mail.failures.emplace(101, network_failure(ErrorCode::remote_gone));
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 20), 0, 0, 1);
    expect(mail.requested == std::vector<std::uint32_t>{101},
        "old pending work is checked even when absent from the latest snapshot");
    expect_pending(store, {});
    expect(db.integer("SELECT retryable FROM sync_pending WHERE uid=101") == 0,
        "confirmed disappearance retires the pending job");
    const auto retained = store.find(id);
    expect(retained && retained->parse_state == ParseState::failed,
        "remote disappearance must not remove an existing local mail");
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 30), 0, 0, 0);
    expect(mail.requested.empty(), "retired jobs must not be retried again");
}

void test_resume_after_interruption(bool after_one_success) {
    TempDirectory directory;
    const auto path = directory.path() / "interrupted.sqlite3";
    ScriptedSource mail;
    mail.snapshot.uids = {103, 102, 101};
    {
        auto store = open_store(path);
        mail.before_fetch = [&](const RemoteKey& key) {
            if (key.uid == (after_one_success ? 102U : 101U)) {
                Database other(path);
                expect(other.integer("SELECT last_discovered_uid FROM sync_state") == 103,
                    "discovery must be committed before starting downloads");
                other.execute("BEGIN IMMEDIATE; ROLLBACK");
                throw Interrupted{};
            }
        };
        bool interrupted = false;
        try {
            (void)sync_detail::sync_inbox(store, config(), mail.source(), 10);
        } catch (const Interrupted&) {
            interrupted = true;
        }
        expect(interrupted, "test should interrupt during download");
        expect_pending(store,
            after_one_success ? std::vector<std::uint32_t>{102, 103}
                              : std::vector<std::uint32_t>{101, 102, 103});
    }
    auto reopened = open_store(path);
    mail.before_fetch = {};
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(reopened, config(), mail.source(), 20),
        0,
        after_one_success ? 2 : 3,
        0);
    expect(mail.requested == (after_one_success ? std::vector<std::uint32_t>{102, 103}
                                                : std::vector<std::uint32_t>{101, 102, 103}),
        "reopening must resume only unfinished downloads");
    expect_pending(reopened, {});
}

void test_empty_snapshot_and_snapshot_failure() {
    TempDirectory directory;
    auto store = open_store(directory.path() / "empty.sqlite3");
    ScriptedSource mail;
    mail.snapshot_error = network_failure();
    mail.snapshot_error->stage = "imap.snapshot";
    const auto failed = sync_detail::sync_inbox(store, config(), mail.source(), 10);
    expect(!failed && failed.error().stage == "imap.snapshot" && mail.requested.empty(),
        "failed snapshot must stop before scheduling or downloading");
    const auto failed_state = store.state("school", "INBOX");
    expect(failed_state && !failed_state->uid_validity && failed_state->last_discovered_uid == 0 &&
               failed_state->last_attempt_at == 10 && !failed_state->last_success_at,
        "snapshot failure should record attempt time without inventing discovery progress");
    mail.snapshot_error.reset();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 20), 0, 0, 0);
    const auto empty = store.state("school", "INBOX");
    expect(empty && empty->uid_validity == 77 && empty->last_success_at == 20,
        "empty mailbox is a successful sync");
}

void test_item_storage_failure_and_global_storage_failure() {
    TempDirectory directory;
    const auto path = directory.path() / "storage-failure.sqlite3";
    auto store = open_store(path);
    Database db(path);
    ScriptedSource mail;
    mail.snapshot.uids = {101, 102, 103};
    db.execute(
        "CREATE TRIGGER reject_one BEFORE INSERT ON messages WHEN NEW.uid=102 "
        "BEGIN SELECT RAISE(ABORT,'one message rejected'); END");
    const auto partial = sync_detail::sync_inbox(store, config(), mail.source(), 10);
    expect_summary(partial, 3, 2, 1);
    expect(partial->failures[0].key.uid == 102 &&
               partial->failures[0].error.code == ErrorCode::storage_error,
        "one failed save should be reported while later messages succeed");
    expect_pending(store, {102});
    expect(db.text("SELECT last_error_code FROM sync_pending WHERE uid=102") == "storage_error",
        "retryable save failure should retain its reason");
    db.execute("DROP TRIGGER reject_one");

    mail.snapshot.uids.push_back(104);
    mail.requested.clear();
    db.execute(
        "CREATE TRIGGER reject_saves BEFORE INSERT ON messages "
        "BEGIN SELECT RAISE(ABORT,'storage unavailable'); END");
    db.execute(
        "CREATE TRIGGER reject_failures BEFORE UPDATE ON sync_pending "
        "BEGIN SELECT RAISE(ABORT,'storage unavailable'); END");
    const auto fatal = sync_detail::sync_inbox(store, config(), mail.source(), 20);
    expect(!fatal && fatal.error().code == ErrorCode::storage_error &&
               mail.requested == std::vector<std::uint32_t>{102},
        "if the failure itself cannot be persisted, abort before downloading another message");
    expect_pending(store, {102, 104});
    db.execute("DROP TRIGGER reject_saves; DROP TRIGGER reject_failures");
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 30), 0, 2, 0);
    expect(mail.requested == std::vector<std::uint32_t>{102, 104},
        "resume durable work after recovery");
}

void test_discovery_failure_stops_before_download() {
    TempDirectory directory;
    const auto path = directory.path() / "discovery-failure.sqlite3";
    auto store = open_store(path);
    Database db(path);
    ScriptedSource mail;
    mail.snapshot.uids = {101};
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 10), 1, 1, 0);
    db.execute(
        "CREATE TRIGGER reject_discovery BEFORE INSERT ON sync_pending "
        "BEGIN SELECT RAISE(ABORT,'discovery unavailable'); END");
    mail.snapshot.uids.push_back(102);
    mail.requested.clear();
    const auto fatal = sync_detail::sync_inbox(store, config(), mail.source(), 20);
    expect(!fatal && fatal.error().stage == "storage.schedule" && mail.requested.empty(),
        "failed discovery must prevent downloads");
    const auto state = store.state("school", "INBOX");
    expect(state && state->last_discovered_uid == 101 && state->last_success_at == 10,
        "failed transaction must preserve cursor and last success");
    expect_pending(store, {});
    db.execute("DROP TRIGGER reject_discovery");
    auto readonly = SqliteStore::open(path, OpenMode::read_only);
    expect(readonly.has_value(), "read-only store should open");
    const auto snapshots = mail.snapshots;
    expect(!sync_detail::sync_inbox(*readonly, config(), mail.source(), 30) &&
               mail.snapshots == snapshots,
        "unwritable database should be detected before contacting the source");
}

void test_source_key_mismatch_and_outcome_failure() {
    TempDirectory directory;
    const auto path = directory.path() / "source-and-outcome.sqlite3";
    auto store = open_store(path);
    Database db(path);
    ScriptedSource mail;
    mail.snapshot.uids = {101, 102};
    auto source = mail.source();
    auto normal_fetch = source.fetch;
    source.fetch = [normal_fetch](const RuntimeConfig& settings, const RemoteKey& key) {
        auto result = normal_fetch(settings, key);
        if (result && key.uid == 101)
            result->key.uid = 999;
        return result;
    };
    const auto partial = sync_detail::sync_inbox(store, config(), source, 10);
    expect_summary(partial, 2, 1, 1);
    expect(partial->failures[0].key.uid == 101 &&
               partial->failures[0].error.code == ErrorCode::protocol_error,
        "a mismatched returned key must not complete the wrong job");
    expect(db.integer("SELECT count(*) FROM messages WHERE uid=999") == 0,
        "unexpected remote mail must not be saved");
    expect_pending(store, {101});
    db.execute(
        "CREATE TRIGGER reject_outcome BEFORE UPDATE OF last_attempt_at ON sync_state "
        "BEGIN SELECT RAISE(ABORT,'outcome unavailable'); END");
    const auto failed = sync_detail::sync_inbox(store, config(), mail.source(), 20);
    expect(!failed && failed.error().stage == "storage.outcome",
        "failure to persist the outcome must not report a successful round");
    expect_pending(store, {});
    expect(db.integer("SELECT count(*) FROM messages") == 2,
        "committed messages must survive an outcome write failure");
    db.execute("DROP TRIGGER reject_outcome");
    mail.requested.clear();
    expect_summary(sync_detail::sync_inbox(store, config(), mail.source(), 30), 0, 0, 0);
    expect(
        mail.requested.empty(), "committed messages must not be downloaded after outcome failure");
}

}  // namespace

int main() {
    try {
        test_initial_limit_and_incremental_recovery();
        test_history_backfill_keeps_incremental_sync_and_retries();
        test_history_backfill_on_first_sync_and_reindex();
        test_uidvalidity_change_retires_old_range();
        test_disappeared_mail_preserves_local_record();
        test_resume_after_interruption(false);
        test_resume_after_interruption(true);
        test_empty_snapshot_and_snapshot_failure();
        test_item_storage_failure_and_global_storage_failure();
        test_discovery_failure_stops_before_download();
        test_source_key_mismatch_and_outcome_failure();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

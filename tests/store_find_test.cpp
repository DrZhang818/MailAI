#include <array>
#include <iostream>
#include <limits>
#include <utility>

#include "mailai/store.h"
#include "store_test_support.h"

namespace {
using namespace mailai;
using namespace store_test;
constexpr std::array modes{OpenMode::read_only, OpenMode::read_write_create};

void seed(const std::filesystem::path& path) {
    expect(SqliteStore::open(path, OpenMode::read_write_create).has_value(),
        "test database should initialize");
    Database db(path);
    insert_account(db);
    db.execute(R"sql(
        INSERT INTO messages(
            id,account_id,sender,subject,body,received_at_utc,folder,uid_validity,uid,
            raw,parse_state,parse_error,historical,message_id,in_reply_to,references_header
        ) VALUES
            (42,'school','sender@example.invalid','课程通知',
             CAST(X'426F6479007461696C' AS TEXT),200,'INBOX',4294967295,4294967295,
             X'610062','parsed','',1,'<42@example.invalid>','<parent@example.invalid>',
             '<root@example.invalid> <parent@example.invalid>'),
            (43,'school','failed@example.invalid','(解析失败)','',100,'INBOX',100,43,
             X'61','failed','test parse failure',0,'','',''),
            (44,'school','empty@example.invalid','无附件的邮件','正文',50,'INBOX',100,44,
             X'61','parsed','',0,'','','');
        INSERT INTO attachments VALUES
            (42,2,'second.txt','text/plain',9223372036854775807),
            (42,0,'课程.pdf','application/pdf',1234),
            (43,0,'failure.dat','application/octet-stream',0);
    )sql");
}

void expect_error(const Result<Mail>& result, ErrorCode code, MailId id) {
    expect(!result && result.error().code == code, "find should return the expected error code");
    expect(result.error().mail_id == id && result.error().stage == "storage.find",
        "find error should identify the input mail ID and query stage");
}

void expect_full_mail(const Mail& mail) {
    expect(mail.id == 42 && mail.account_id == "school", "mail identity should match");
    expect(mail.sender == "sender@example.invalid" && mail.subject == "课程通知",
        "sender and subject should be mapped correctly");
    expect(mail.body == std::string{"Body\0tail", 9} && mail.received_at_utc == 200,
        "body bytes and received time should be preserved");
    expect(mail.folder == "INBOX" && mail.uid_validity == 4294967295U && mail.uid == 4294967295U,
        "remote location should retain the full uint32 range");
    expect(mail.parse_state == ParseState::parsed && mail.parse_error.empty() && mail.historical,
        "parse and historical states should be preserved");
    expect(mail.message_id == "<42@example.invalid>" &&
               mail.in_reply_to == "<parent@example.invalid>" &&
               mail.references == "<root@example.invalid> <parent@example.invalid>",
        "thread headers should be mapped correctly");
    expect(mail.attachments.size() == 2, "only this mail's attachments should be returned");
    expect(mail.attachments[0].name == "课程.pdf" &&
               mail.attachments[0].media_type == "application/pdf" &&
               mail.attachments[0].size_bytes == 1234,
        "first attachment should follow ordinal order");
    expect(mail.attachments[1].name == "second.txt" &&
               mail.attachments[1].media_type == "text/plain" &&
               mail.attachments[1].size_bytes ==
                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
        "second attachment should retain its metadata and 64-bit size");
}

void test_full_details_and_owned_values() {
    TempDirectory directory;
    const auto path = directory.path() / "details.sqlite3";
    seed(path);
    for (auto mode : modes) {
        Mail saved;
        {
            auto store = SqliteStore::open(path, mode);
            expect(store.has_value(), "details database should reopen");
            auto result = store->find(42);
            expect(result.has_value(), "existing mail should be returned");
            saved = std::move(*result);
            const auto repeated = store->find(42);
            expect(repeated.has_value(), "successful query must leave the connection reusable");
            expect_full_mail(*repeated);
        }
        expect_full_mail(saved);
    }
}

void test_absent_mail_empty_attachments_and_failed_parse() {
    TempDirectory directory;
    const auto path = directory.path() / "cases.sqlite3";
    seed(path);
    auto store = SqliteStore::open(path, OpenMode::read_only);
    expect(store.has_value(), "test database should open");
    expect_error(store->find(0), ErrorCode::invalid_argument, 0);
    expect_error(store->find(999), ErrorCode::mail_not_found, 999);
    const auto empty = store->find(44);
    expect(empty.has_value() && empty->subject == "无附件的邮件" && empty->attachments.empty(),
        "mail without attachments should still succeed");
    const auto failed = store->find(43);
    expect(failed.has_value() && failed->parse_state == ParseState::failed &&
               failed->parse_error == "test parse failure" && failed->subject == "(解析失败)" &&
               failed->body.empty() && !failed->historical,
        "stored parse failure should be returned as a successful read of that record");
    expect(failed->attachments.size() == 1 && failed->attachments[0].name == "failure.dat" &&
               failed->attachments[0].size_bytes == 0,
        "zero-sized attachment metadata should be preserved");
}

void test_id_range_without_truncation() {
    TempDirectory directory;
    const auto path = directory.path() / "range.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO messages(
            id,account_id,folder,uid_validity,uid,sender,subject,body,
            received_at_utc,raw,parse_state
        ) VALUES
            (9223372036854775807,'school','INBOX',100,50,'sender','largest','body',1,X'61','parsed'),
            (-1,'school','INBOX',100,51,'sender','negative sentinel','body',1,X'61','parsed');
    )sql");
    for (auto mode : modes) {
        auto store = SqliteStore::open(path, mode);
        expect(store.has_value(), "range database should reopen");
        const auto largest = static_cast<MailId>(std::numeric_limits<std::int64_t>::max());
        const auto found = store->find(largest);
        expect(found.has_value() && found->id == largest && found->subject == "largest",
            "INT64_MAX should remain a queryable mail ID");
        expect_error(store->find(largest + 1), ErrorCode::mail_not_found, largest + 1);
        const auto maximum = std::numeric_limits<MailId>::max();
        expect_error(store->find(maximum), ErrorCode::mail_not_found, maximum);
    }
}

void test_queries_preserve_records() {
    TempDirectory directory;
    const auto path = directory.path() / "unchanged.sqlite3";
    seed(path);
    Database db(path);
    const auto catalog = db.catalog();
    const auto messages = db.rows("SELECT * FROM messages ORDER BY id");
    const auto attachments = db.rows("SELECT * FROM attachments ORDER BY mail_id,ordinal");
    for (auto mode : modes) {
        auto store = SqliteStore::open(path, mode);
        expect(store.has_value(), "database should open");
        expect(store->find(42).has_value(), "details query should succeed");
        expect_error(store->find(999), ErrorCode::mail_not_found, 999);
    }
    expect(db.catalog() == catalog && db.rows("SELECT * FROM messages ORDER BY id") == messages &&
               db.rows("SELECT * FROM attachments ORDER BY mail_id,ordinal") == attachments &&
               db.integer("PRAGMA user_version") == 1,
        "successful and missing details queries must preserve schema and records");
}

void test_attachment_query_failure_and_recovery() {
    TempDirectory directory;
    const auto path = directory.path() / "recovery.sqlite3";
    seed(path);
    auto store = SqliteStore::open(path, OpenMode::read_only);
    expect(store.has_value(), "database should open before an external schema change");
    Database db(path);
    const auto definition = db.text("SELECT sql FROM sqlite_schema WHERE name='attachments'");
    db.execute("DROP TABLE attachments");
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto failed = store->find(42);
        expect(!failed && failed.error().code == ErrorCode::storage_error &&
                   failed.error().stage == "storage.find",
            "failure of the second query should return an error and release its transaction");
    }
    db.execute(definition);
    const auto recovered = store->find(42);
    expect(recovered.has_value() && recovered->attachments.empty(),
        "query should recover after the test fixture restores the table");
    expect(
        recovered->body == std::string{"Body\0tail", 9}, "failure must not change the mail body");
}

void test_invalid_stored_fields() {
    for (const auto* mutation :
        {"UPDATE messages SET uid=0 WHERE id=42",
            "UPDATE messages SET uid_validity=4294967296 WHERE id=42",
            "UPDATE messages SET parse_state='invalid' WHERE id=42",
            "UPDATE attachments SET size_bytes=-1 WHERE mail_id=42 AND ordinal=2"}) {
        TempDirectory directory;
        const auto path = directory.path() / "invalid.sqlite3";
        seed(path);
        Database db(path);
        db.execute("PRAGMA ignore_check_constraints=ON");
        db.execute(mutation);
        auto store = SqliteStore::open(path, OpenMode::read_only);
        expect(store.has_value(), "opening checks structure rather than scanning every record");
        for (int attempt = 0; attempt < 2; ++attempt)
            expect_error(store->find(42), ErrorCode::storage_error, 42);
    }
}
}  // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[]{
        {"full_details_and_owned_values", test_full_details_and_owned_values},
        {"absent_mail_empty_attachments_and_failed_parse",
            test_absent_mail_empty_attachments_and_failed_parse},
        {"id_range_without_truncation", test_id_range_without_truncation},
        {"queries_preserve_records", test_queries_preserve_records},
        {"attachment_query_failure_and_recovery", test_attachment_query_failure_and_recovery},
        {"invalid_stored_fields", test_invalid_stored_fields}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

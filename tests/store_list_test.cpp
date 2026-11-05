#include <array>
#include <iostream>
#include <utility>

#include "mailai/store.h"
#include "store_test_support.h"

namespace {
using namespace mailai;
using namespace store_test;
constexpr std::array modes{OpenMode::read_only, OpenMode::read_write_create};

void initialize(const std::filesystem::path& path) {
    expect(SqliteStore::open(path, OpenMode::read_write_create).has_value(),
        "test database should initialize");
}

void seed(const std::filesystem::path& path) {
    initialize(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO accounts VALUES
            ('school','学校邮箱','school@example.invalid','imap.example.invalid',993,
             'school','TEST_PASSWORD_REF','implicit'),
            ('other','另一个邮箱','other@example.invalid','imap.example.invalid',993,
             'other','TEST_PASSWORD_REF','implicit');
        INSERT INTO messages(
            id,account_id,folder,uid_validity,uid,sender,subject,body,
            received_at_utc,raw,parse_state,parse_error,historical
        ) VALUES
            (44,'school','INBOX',100,44,'library@example.invalid','图书馆通知','正文',
             100,X'610062','parsed','',1),
            (7,'school','INBOX',100,7,'older@example.invalid','较早的邮件','旧正文',
             50,X'61','parsed','',0),
            (43,'other','INBOX',100,43,'sender@example.invalid','(解析失败)','',
             200,X'61','failed','test parse failure',1),
            (42,'school','INBOX',100,42,'course@example.invalid','课程通知','正文',
             200,X'61','parsed','',0);
    )sql");
}

void expect_item(const InboxItem& actual, const InboxItem& expected) {
    expect(actual.id == expected.id, "mail ID should match");
    expect(actual.account_id == expected.account_id, "account ID should match");
    expect(actual.account_name == expected.account_name, "account name should come from accounts");
    expect(actual.sender == expected.sender, "sender should match");
    expect(actual.subject == expected.subject, "subject should match");
    expect(actual.received_at_utc == expected.received_at_utc, "received time should match");
    expect(actual.parse_state == expected.parse_state, "parse state should be preserved");
    expect(actual.historical == expected.historical, "historical state should be preserved");
}

void test_empty_and_unknown_accounts() {
    TempDirectory directory;
    const auto path = directory.path() / "empty.sqlite3";
    initialize(path);
    for (auto mode : modes) {
        auto store = SqliteStore::open(path, mode);
        expect(store.has_value(), "empty version 1 should open");
        const auto result = store->list(InboxQuery{});
        expect(result.has_value() && result->empty(), "empty database should return an empty list");
    }
    Database db(path);
    insert_account(db);
    for (auto mode : modes) {
        auto store = SqliteStore::open(path, mode);
        expect(store.has_value(), "known account database should open");
        const auto known = store->list(InboxQuery{AccountId{"school"}});
        expect(known.has_value() && known->empty(), "known empty account should succeed");
        for (const auto* id : {"missing", "", "all", "' OR 1=1 --"}) {
            const auto result = store->list(InboxQuery{AccountId{id}});
            expect(!result && result.error().code == ErrorCode::unknown_account,
                "unknown account should be distinguished from an empty inbox");
            expect(result.error().account_id == AccountId{id} &&
                       result.error().stage == "storage.list",
                "unknown account should include query context");
        }
        const auto repeated = store->list(InboxQuery{AccountId{"school"}});
        expect(repeated.has_value() && repeated->empty(),
            "query should remain usable after unknown account errors");
    }
}

void test_all_fields_order_and_filter() {
    TempDirectory directory;
    const auto path = directory.path() / "list.sqlite3";
    seed(path);
    const std::vector<InboxItem> expected{
        {42,
            "school",
            "学校邮箱",
            "course@example.invalid",
            "课程通知",
            200,
            ParseState::parsed,
            false},
        {43,
            "other",
            "另一个邮箱",
            "sender@example.invalid",
            "(解析失败)",
            200,
            ParseState::failed,
            true},
        {44,
            "school",
            "学校邮箱",
            "library@example.invalid",
            "图书馆通知",
            100,
            ParseState::parsed,
            true},
        {7,
            "school",
            "学校邮箱",
            "older@example.invalid",
            "较早的邮件",
            50,
            ParseState::parsed,
            false}
    };
    for (auto mode : modes) {
        auto store = SqliteStore::open(path, mode);
        expect(store.has_value(), "populated database should reopen");
        const auto all = store->list(InboxQuery{});
        expect(all.has_value() && all->size() == expected.size(), "all mail should be returned");
        for (std::size_t i = 0; i < expected.size(); ++i) expect_item((*all)[i], expected[i]);
        const auto school = store->list(InboxQuery{AccountId{"school"}});
        expect(
            school.has_value() && school->size() == 3, "account filter should exclude other mail");
        expect_item((*school)[0], expected[0]);
        expect_item((*school)[1], expected[2]);
        expect_item((*school)[2], expected[3]);
        const auto other = store->list(InboxQuery{AccountId{"other"}});
        expect(other.has_value() && other->size() == 1, "other account should have one result");
        expect_item((*other)[0], expected[1]);
    }
}

void test_bound_account_and_owned_text() {
    TempDirectory directory;
    const auto path = directory.path() / "quoted.sqlite3";
    seed(path);
    Database db(path);
    db.execute(R"sql(
        INSERT INTO accounts VALUES
            ('team''s','带引号的账号','team@example.invalid','imap.example.invalid',993,
             'team','TEST_PASSWORD_REF','implicit');
        INSERT INTO messages(
            id,account_id,folder,uid_validity,uid,sender,subject,body,
            received_at_utc,raw,parse_state
        ) VALUES(91,'team''s','INBOX',100,91,'team@example.invalid',
                 CAST(X'7375626A656374007461696C' AS TEXT),'正文',300,X'61','parsed');
    )sql");
    std::vector<InboxItem> saved;
    {
        auto store = SqliteStore::open(path, OpenMode::read_only);
        expect(store.has_value(), "quoted account database should open");
        auto result = store->list(InboxQuery{AccountId{"team's"}});
        expect(result.has_value() && result->size() == 1, "quoted account should be bound as data");
        saved = std::move(*result);
        const auto unknown = store->list(InboxQuery{AccountId{"team's' OR 1=1 --"}});
        expect(!unknown && unknown.error().code == ErrorCode::unknown_account,
            "SQL-looking account input must not broaden the query");
    }
    expect_item(saved[0],
        {91,
            "team's",
            "带引号的账号",
            "team@example.invalid",
            std::string{"subject\0tail", 12},
            300,
            ParseState::parsed,
            false});
}

void test_queries_preserve_data_and_see_account_renames() {
    TempDirectory directory;
    const auto path = directory.path() / "unchanged.sqlite3";
    seed(path);
    Database db(path);
    const auto catalog = db.catalog();
    const auto accounts = db.rows("SELECT * FROM accounts ORDER BY id");
    const auto messages = db.rows("SELECT * FROM messages ORDER BY id");
    auto store = SqliteStore::open(path, OpenMode::read_only);
    expect(store.has_value(), "database should open read-only");
    expect(store->list(InboxQuery{}).has_value(), "read-only listing should work");
    expect(db.catalog() == catalog && db.rows("SELECT * FROM accounts ORDER BY id") == accounts &&
               db.rows("SELECT * FROM messages ORDER BY id") == messages &&
               db.integer("PRAGMA user_version") == 1,
        "listing must preserve schema, version and records");
    db.execute("UPDATE accounts SET display_name='新的显示名称' WHERE id='school'");
    const auto renamed = store->list(InboxQuery{AccountId{"school"}});
    expect(renamed.has_value() && renamed->size() == 3, "listing should work after account rename");
    for (const auto& item : *renamed)
        expect(
            item.account_name == "新的显示名称", "each query should read the current account name");
}

void test_sqlite_errors_are_reported_and_transactions_end() {
    TempDirectory directory;
    const auto path = directory.path() / "sql-error.sqlite3";
    seed(path);
    auto store = SqliteStore::open(path, OpenMode::read_only);
    expect(store.has_value(), "database should open before schema is changed externally");
    Database db(path);
    db.execute("DROP TABLE messages");
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = store->list(InboxQuery{});
        expect(!result && result.error().code == ErrorCode::storage_error &&
                   result.error().stage == "storage.list" && !result.error().message.empty(),
            "SQL failures should be reported without leaking an active read transaction");
    }
}

void test_invalid_rows_do_not_return_partial_results() {
    for (const auto* mutation :
        {"UPDATE messages SET id=-7 WHERE id=7",
            "PRAGMA ignore_check_constraints=ON; "
            "UPDATE messages SET parse_state='invalid' WHERE id=7"}) {
        TempDirectory directory;
        const auto path = directory.path() / "invalid-row.sqlite3";
        seed(path);
        Database db(path);
        db.execute(mutation);
        auto store = SqliteStore::open(path, OpenMode::read_only);
        expect(store.has_value(), "structure validation should not scan every stored mail");
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto result = store->list(InboxQuery{});
            expect(!result && result.error().code == ErrorCode::storage_error &&
                       result.error().stage == "storage.list",
                "invalid stored fields should reject the result, not return partial mail");
        }
    }
}
}  // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[]{
        {"empty_and_unknown_accounts", test_empty_and_unknown_accounts},
        {"all_fields_order_and_filter", test_all_fields_order_and_filter},
        {"bound_account_and_owned_text", test_bound_account_and_owned_text},
        {"queries_preserve_data_and_see_account_renames",
            test_queries_preserve_data_and_see_account_renames},
        {"sqlite_errors_are_reported_and_transactions_end",
            test_sqlite_errors_are_reported_and_transactions_end},
        {"invalid_rows_do_not_return_partial_results",
            test_invalid_rows_do_not_return_partial_results}
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

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "store/sqlite_utils.h"
#include "store_test_support.h"

namespace {
using namespace mailai;
using namespace mailai::store_detail;
using namespace store_test;

void expect_failure(const auto& result, std::string_view stage) {
    expect(!result && result.error().code == ErrorCode::storage_error,
        "SQL operation should return a storage error");
    expect(result.error().account_id == "school" && !result.error().mail_id &&
               result.error().stage == stage,
        "SQL failures should retain the account and the correct operation stage");
}

void test_values_are_copied_and_preserve_sqlite_types() {
    Database db(":memory:");
    db.execute(
        "CREATE TABLE samples(id INTEGER PRIMARY KEY, text_value TEXT NOT NULL, "
        "blob_value BLOB NOT NULL, int_value INTEGER, optional_value INTEGER)");
    const SqlContext sql{db.get(), "storage.test", "school"};
    std::string text{"hello\0tail", 10};
    std::string bytes{"raw\0bytes", 9};
    auto insert = sql.prepare("INSERT INTO samples VALUES (?1,?2,?3,?4,?5)",
        {1, text, BlobView{bytes}, std::numeric_limits<std::int64_t>::max(), nullptr});
    expect(insert.has_value(), "text and blob parameters should bind");
    text.assign("changed");
    bytes.assign("changed");
    expect(sql.execute(*insert).has_value(), "prepared values should survive input changes");
    expect(
        sql.execute("INSERT INTO samples VALUES (?1,?2,?3,?4,?5)",
               {2, std::string_view{}, BlobView{}, std::numeric_limits<std::int64_t>::min(), true})
            .has_value(),
        "empty values, NULL and integer boundaries should bind without loss");

    auto rows = sql.prepare(
        "SELECT text_value,blob_value,int_value,optional_value,"
        "typeof(text_value),typeof(blob_value) FROM samples ORDER BY id");
    expect(rows.has_value(), "stored values should be queryable");
    auto next = sql.next(*rows);
    expect(next && *next, "first row should be returned");
    expect(column_text(rows->get(), 0) == std::string{"hello\0tail", 10} &&
               column_text(rows->get(), 1) == std::string{"raw\0bytes", 9} &&
               sqlite3_column_int64(rows->get(), 2) == std::numeric_limits<std::int64_t>::max() &&
               sqlite3_column_type(rows->get(), 3) == SQLITE_NULL,
        "binding must copy complete bytes and retain integer and NULL values");
    next = sql.next(*rows);
    expect(next && *next, "second row should be returned");
    expect(column_text(rows->get(), 0).empty() && sqlite3_column_bytes(rows->get(), 1) == 0 &&
               column_text(rows->get(), 4) == "text" && column_text(rows->get(), 5) == "blob" &&
               sqlite3_column_int64(rows->get(), 2) == std::numeric_limits<std::int64_t>::min() &&
               sqlite3_column_int64(rows->get(), 3) == 1,
        "empty TEXT and BLOB must remain distinct from NULL");
    expect(sql.finish(*rows).has_value(), "a fully read query should finish");

    auto absent = sql.prepare("SELECT id FROM samples WHERE id=?1", {999});
    expect(absent.has_value(), "empty query should prepare");
    const auto empty = sql.next(*absent);
    expect(empty && !*empty, "no rows should be a successful end of query");
}

void test_reused_writes_keep_constant_bindings_and_recover_from_bind_errors() {
    Database db(":memory:");
    db.execute("CREATE TABLE jobs(scope TEXT NOT NULL, uid INTEGER PRIMARY KEY)");
    const SqlContext sql{db.get(), "storage.test", "school"};
    auto insert = sql.prepare("INSERT INTO jobs VALUES (?1,?2)", {"INBOX"});
    expect(insert.has_value(), "partially bound reusable statement should prepare");
    for (const auto uid : std::array{7, 8}) {
        expect(sql.bind(*insert, 2, {uid}).has_value(), "changing UID should bind");
        expect(sql.execute(*insert).has_value(), "write should reset for its next execution");
    }
    expect_failure(sql.bind(*insert, 3, {9}), "storage.test");
    expect(sql.bind(*insert, 2, {9}).has_value() && sql.execute(*insert).has_value(),
        "a failed bind must not prevent a later valid execution");
    expect(
        db.rows("SELECT scope,uid FROM jobs ORDER BY uid") ==
            std::vector<std::vector<std::string>>{{"INBOX", "7"}, {"INBOX", "8"}, {"INBOX", "9"}},
        "reset must retain bindings that callers intentionally reuse");
    expect_failure(sql.execute(*insert), "storage.test");
    expect(sql.execute("INSERT INTO jobs VALUES (?1,?2)", {"INBOX", 10}).has_value(),
        "the connection must remain usable after a constraint error");
}

void test_prepare_iteration_and_execution_errors_keep_context() {
    Database db(":memory:");
    const SqlContext sql{db.get(), "storage.test", "school"};
    expect_failure(sql.prepare("SELECT FROM"), "storage.test");
    expect_failure(sql.execute("SELECT 1"), "storage.test");
    auto failing = sql.prepare("SELECT abs(-9223372036854775808)");
    expect(failing.has_value(), "a runtime SQL error should not fail preparation");
    expect_failure(sql.next(*failing), "storage.test");
    auto rows = sql.prepare("SELECT 1 UNION ALL SELECT 2");
    expect(rows.has_value(), "multi-row query should prepare");
    const auto first = sql.next(*rows);
    expect(first && *first, "first row should be readable");
    expect_failure(sql.finish(*rows), "storage.test");
    expect(sql.execute("CREATE TABLE valid(id INTEGER)").has_value(),
        "errors must not leave the connection unusable");
}

void test_transaction_failures_preserve_stage_and_rollback() {
    Database db(":memory:");
    db.execute("PRAGMA foreign_keys=ON");
    db.execute("CREATE TABLE parent(id INTEGER PRIMARY KEY)");
    db.execute(
        "CREATE TABLE child(parent_id INTEGER REFERENCES parent(id) "
        "DEFERRABLE INITIALLY DEFERRED)");
    const SqlContext sql{db.get(), "storage.test", "school"};
    {
        auto transaction = sql.begin(TransactionMode::write);
        expect(transaction.has_value(), "write transaction should begin");
        expect_failure(sql.begin(TransactionMode::write), "storage.transaction");
        expect(sql.execute("INSERT INTO child VALUES (?1)", {7}).has_value(),
            "deferred foreign key should allow insertion before commit");
        expect_failure(sql.commit(*transaction), "storage.transaction");
    }
    expect(db.integer("SELECT count(*) FROM child") == 0,
        "failed commit should roll back when its transaction leaves scope");
    auto transaction = sql.begin(TransactionMode::write);
    expect(transaction.has_value(), "connection should accept a new transaction");
    expect(sql.execute("INSERT INTO parent VALUES (?1)", {7}).has_value() &&
               sql.execute("INSERT INTO child VALUES (?1)", {7}).has_value() &&
               sql.commit(*transaction).has_value(),
        "a valid transaction should commit after recovery");
    expect(db.integer("SELECT count(*) FROM child") == 1, "committed data should remain");
}
}  // namespace

int main() {
    try {
        test_values_are_copied_and_preserve_sqlite_types();
        test_reused_writes_keep_constant_bindings_and_recover_from_bind_errors();
        test_prepare_iteration_and_execution_errors_keep_context();
        test_transaction_failures_preserve_stage_and_rollback();
        std::cout << "sqlite_context tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include <array>
#include <iostream>
#include <limits>
#include <ranges>
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
    expect(sql.execute("INSERT INTO samples VALUES (?1,?2,?3,?4,?5)",
               {1, text, BlobView{bytes}, std::numeric_limits<std::int64_t>::max(), nullptr})
               .has_value(),
        "complete text and blob values should bind through the SQL interface");
    text.assign("changed");
    bytes.assign("changed");
    expect(sql.execute("INSERT INTO samples VALUES (?1,?2,?3,?4,?5)",
               {2, std::string_view{}, BlobView{}, std::numeric_limits<std::int64_t>::min(), true})
               .has_value(),
        "empty values, NULL and integer boundaries should bind without loss");

    expect(db.rows("SELECT hex(text_value),hex(blob_value),int_value,optional_value,"
                   "typeof(text_value),typeof(blob_value) FROM samples ORDER BY id") ==
               std::vector<std::vector<std::string>>{
                   {"68656C6C6F007461696C", "726177006279746573", "9223372036854775807", "<NULL>", "text", "blob"},
                   {"", "", "-9223372036854775808", "1", "text", "blob"}},
        "binding must preserve complete bytes, SQLite types and integer values");
}

void test_batch_prepares_once_and_copies_common_parameters() {
    Database db(":memory:");
    db.execute("CREATE TABLE jobs(scope TEXT NOT NULL, uid INTEGER PRIMARY KEY)");
    const SqlContext sql{db.get(), "storage.test", "school"};
    int prepared = 0;
    expect(sqlite3_set_authorizer(db.get(), [](void* count, int action, const char* table,
               const char*, const char*, const char*) {
               if (action == SQLITE_INSERT && table && std::string_view{table} == "jobs")
                   ++*static_cast<int*>(count);
               return SQLITE_OK;
           }, &prepared) == SQLITE_OK,
        "preparation observer should install");

    std::string scope = "INBOX";
    const std::array uids{7, 8};
    auto rows = uids | std::views::transform([&](int uid) {
        scope.assign("OTHER");
        return std::array<SqlValue, 1>{uid};
    });
    auto inserted = sql.execute_many("INSERT INTO jobs VALUES (?1,?2)", {scope}, rows);
    expect(sqlite3_set_authorizer(db.get(), nullptr, nullptr) == SQLITE_OK,
        "preparation observer should uninstall");
    expect(inserted.has_value() && prepared == 1, "a batch should prepare its SQL exactly once");
    expect(db.rows("SELECT scope,uid FROM jobs ORDER BY uid") ==
               std::vector<std::vector<std::string>>{{"INBOX", "7"}, {"INBOX", "8"}},
        "reset must retain copied common parameters while each row changes its suffix");

    expect_failure(sql.execute("INSERT INTO jobs VALUES (?1,?2)", {"INBOX", 9, 10}), "storage.test");
    expect(sql.execute("INSERT INTO jobs VALUES (?1,?2)", {"INBOX", 9}).has_value(),
        "a failed bind must not prevent later valid execution");
    expect_failure(sql.execute_many("INSERT INTO jobs VALUES (?1,?2)", {"INBOX"}, rows), "storage.test");
    expect(sql.execute("INSERT INTO jobs VALUES (?1,?2)", {"INBOX", 10}).has_value(),
        "a batch constraint failure must leave the connection usable");
}

void test_batch_row_shape_and_transaction_recovery() {
    Database db(":memory:");
    db.execute("CREATE TABLE jobs(scope TEXT NOT NULL, uid INTEGER PRIMARY KEY, label TEXT NOT NULL)");
    const SqlContext sql{db.get(), "storage.test", "school"};
    constexpr auto insert = "INSERT INTO jobs VALUES (?1,?2,?3)";
    const std::vector<std::vector<SqlValue>> incomplete{{1, "first"}, {2}};
    {
        auto transaction = sql.begin(TransactionMode::write);
        expect(transaction.has_value(), "batch transaction should begin");
        expect_failure(sql.execute_many(insert, {"INBOX"}, incomplete), "storage.test");
    }
    expect(db.integer("SELECT count(*) FROM jobs") == 0,
        "a later malformed batch row should roll back the caller's entire transaction");

    const std::vector<std::vector<SqlValue>> valid{{1, "first"}, {2, "second"}};
    auto transaction = sql.begin(TransactionMode::write);
    expect(transaction && sql.execute_many(insert, {"INBOX"}, valid) && sql.commit(*transaction),
        "a complete batch should commit after recovery");
    expect(db.rows("SELECT uid,label FROM jobs ORDER BY uid") ==
               std::vector<std::vector<std::string>>{{"1", "first"}, {"2", "second"}},
        "each batch row must replace all varying parameters");

    const std::vector<std::array<SqlValue, 2>> empty;
    expect(sql.execute_many(insert, {"INBOX"}, empty).has_value(), "an empty batch should perform no writes");
    expect_failure(sql.execute_many("INSERT INTO missing VALUES (?1,?2)", {}, empty), "storage.test");
}

void test_query_and_execution_errors_keep_context() {
    Database db(":memory:");
    const SqlContext sql{db.get(), "storage.test", "school"};
    expect_failure(sql.query_integer("SELECT FROM"), "storage.test");
    expect_failure(sql.execute("SELECT 1"), "storage.test");
    expect_failure(sql.query_integer("SELECT abs(-9223372036854775808)"), "storage.test");
    expect_failure(sql.query_integer("SELECT 1 UNION ALL SELECT 2"), "storage.test");
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

struct Sample {
    std::string text;
    std::int64_t integer{};
    std::optional<std::int64_t> optional;
    std::uint32_t uid{};
    std::uint64_t size{};
    bool flag{};
};

Result<Sample> decode_sample(const SqlRow& row) {
    Sample sample;
    auto decoded = row.read(sample.text, sample.integer, sample.optional, sample.uid, sample.size, sample.flag);
    if (!decoded) return std::unexpected(decoded.error());
    return sample;
}

void test_typed_queries_preserve_values_and_own_their_results() {
    std::optional<Sample> saved;
    {
        Database db(":memory:");
        const SqlContext sql{db.get(), "storage.test", "school"};
        auto result = sql.query_one(
            "SELECT CAST(X'610062' AS TEXT), -9223372036854775808, NULL, "
            "4294967295, 9223372036854775807, 1",
            {}, decode_sample);
        expect(result && result->has_value(), "one row should decode into an owned result");
        saved = std::move(*result);

        auto optional_values = sql.query_all(
            "SELECT NULL UNION ALL SELECT 0 UNION ALL SELECT -1", {}, decode_scalar<std::optional<std::int64_t>>);
        expect(optional_values && *optional_values == std::vector<std::optional<std::int64_t>>{std::nullopt, 0, -1},
            "optional decoding must distinguish NULL from zero and negative values");

        auto missing = sql.query_one("SELECT 1 WHERE 0", {}, decode_scalar<std::int64_t>);
        expect(missing && !*missing, "a single-row query should distinguish no row from failure");
        auto empty = sql.query_all("SELECT 1 WHERE 0", {}, decode_scalar<std::int64_t>);
        expect(empty && empty->empty(), "a multi-row query should return an empty collection");
    }
    expect(saved && saved->text == std::string{"a\0b", 3} &&
               saved->integer == std::numeric_limits<std::int64_t>::min() && !saved->optional &&
               saved->uid == 4294967295U && saved->size == 9223372036854775807ULL && saved->flag,
        "decoded values must retain full bytes and integer ranges after the connection closes");
}

void test_typed_queries_reject_bad_rows_and_extra_results() {
    Database db(":memory:");
    const SqlContext sql{db.get(), "storage.test", "school"};
    for (const auto* query : {"SELECT '7'", "SELECT 1.0", "SELECT NULL", "SELECT 1, 2"}) {
        expect_failure(sql.query_one(query, {}, decode_scalar<std::int64_t>), "storage.test");
    }
    expect_failure(sql.query_one("SELECT -1", {}, decode_scalar<std::uint64_t>), "storage.test");
    expect_failure(sql.query_one("SELECT 4294967296", {}, decode_scalar<std::uint32_t>), "storage.test");
    expect_failure(sql.query_one("SELECT 2", {}, decode_scalar<bool>), "storage.test");
    expect_failure(sql.query_one("SELECT X'61'", {}, decode_scalar<std::string>), "storage.test");
    expect_failure(sql.query_one("SELECT 1 UNION ALL SELECT 2", {}, decode_scalar<std::int64_t>), "storage.test");

    for (int attempt = 0; attempt < 2; ++attempt) {
        expect_failure(sql.query_all("SELECT 1 UNION ALL SELECT 'bad'", {}, decode_scalar<std::int64_t>), "storage.test");
        expect_failure(sql.query_all("SELECT 1 UNION ALL SELECT abs(-9223372036854775808)", {}, decode_scalar<std::int64_t>), "storage.test");
        auto recovered = sql.query_one("SELECT 7", {}, decode_scalar<std::int64_t>);
        expect(recovered && *recovered == 7, "decode and execution failures must leave the connection usable");
    }
}
}  // namespace

int main() {
    try {
        test_values_are_copied_and_preserve_sqlite_types();
        test_batch_prepares_once_and_copies_common_parameters();
        test_batch_row_shape_and_transaction_recovery();
        test_query_and_execution_errors_keep_context();
        test_transaction_failures_preserve_stage_and_rollback();
        test_typed_queries_preserve_values_and_own_their_results();
        test_typed_queries_reject_bad_rows_and_extra_results();
        std::cout << "sqlite_context tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

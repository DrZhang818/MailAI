#include "schema.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "sqlite_utils.h"

namespace mailai::store_detail {
namespace {
// Preserve version 1 SQL text: SQLite stores its internal whitespace.
constexpr std::array version_one_sql{
    R"sql(CREATE TABLE accounts (
                id TEXT PRIMARY KEY NOT NULL,
                display_name TEXT NOT NULL,
                address TEXT NOT NULL,
                imap_host TEXT NOT NULL,
                imap_port INTEGER NOT NULL,
                imap_username TEXT NOT NULL,
                credential_env TEXT NOT NULL,
                tls_mode TEXT NOT NULL
            );)sql",
    R"sql(CREATE TABLE sync_state (
        account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
        folder TEXT NOT NULL CHECK(length(folder)>0),
        uid_validity INTEGER CHECK(uid_validity BETWEEN 1 AND 4294967295),
        last_discovered_uid INTEGER NOT NULL DEFAULT 0 CHECK(last_discovered_uid BETWEEN 0 AND 4294967295),
        last_attempt_at INTEGER,
        last_success_at INTEGER,
        PRIMARY KEY(account_id,folder),
        CHECK(uid_validity IS NOT NULL OR last_discovered_uid=0)
    );)sql",
    R"sql(CREATE TABLE sync_pending (
        account_id TEXT NOT NULL,
        folder TEXT NOT NULL,
        uid_validity INTEGER NOT NULL CHECK(uid_validity BETWEEN 1 AND 4294967295),
        uid INTEGER NOT NULL CHECK(uid BETWEEN 1 AND 4294967295),
        retryable INTEGER NOT NULL DEFAULT 1 CHECK(retryable IN (0,1)),
        last_error_code TEXT,
        last_error_stage TEXT NOT NULL DEFAULT '',
        last_error_message TEXT NOT NULL DEFAULT '',
        PRIMARY KEY(account_id,folder,uid_validity,uid),
        FOREIGN KEY(account_id,folder) REFERENCES sync_state(account_id,folder) ON DELETE RESTRICT,
        CHECK(last_error_code IS NOT NULL OR (last_error_stage='' AND last_error_message=''))
    );)sql",
    R"sql(CREATE TABLE messages (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
        folder TEXT NOT NULL CHECK(length(folder)>0),
        uid_validity INTEGER NOT NULL CHECK(uid_validity BETWEEN 1 AND 4294967295),
        uid INTEGER NOT NULL CHECK(uid BETWEEN 1 AND 4294967295),
        sender TEXT NOT NULL,
        subject TEXT NOT NULL,
        body TEXT NOT NULL,
        received_at_utc INTEGER NOT NULL,
        raw BLOB NOT NULL,
        parse_state TEXT NOT NULL CHECK(parse_state IN ('parsed','failed')),
        parse_error TEXT NOT NULL DEFAULT '',
        historical INTEGER NOT NULL DEFAULT 0 CHECK(historical IN (0,1)),
        message_id TEXT NOT NULL DEFAULT '',
        in_reply_to TEXT NOT NULL DEFAULT '',
        references_header TEXT NOT NULL DEFAULT '',
        UNIQUE(account_id,folder,uid_validity,uid),
        CHECK((parse_state='parsed' AND parse_error='') OR
              (parse_state='failed' AND length(parse_error)>0 AND subject='(解析失败)' AND body=''))
    );)sql",
    R"sql(CREATE TABLE attachments (
        mail_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
        ordinal INTEGER NOT NULL CHECK(ordinal>=0),
        name TEXT NOT NULL,
        media_type TEXT NOT NULL,
        size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),
        PRIMARY KEY(mail_id,ordinal)
    );)sql",
    "CREATE INDEX messages_inbox_order ON messages(received_at_utc DESC,id ASC);",
    "CREATE INDEX messages_account_order ON messages(account_id,received_at_utc DESC,id ASC);"};

struct Object {
    std::string type;
    std::string name;
    std::string table;
    std::optional<std::string> sql;
    bool operator==(const Object&) const = default;
};
using Catalog = std::vector<Object>;

Error schema_error(std::string message) {
    return {ErrorCode::storage_error, std::move(message), std::nullopt, std::nullopt,
            "storage.schema"};
}
std::string text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? std::string{reinterpret_cast<const char*>(value),
                               static_cast<std::size_t>(sqlite3_column_bytes(statement, column))}
                 : std::string{};
}
Result<Catalog> read_catalog(sqlite3* db) {
    auto statement = prepare(db,
                             "SELECT type,name,tbl_name,sql FROM main.sqlite_schema "
                             "WHERE name NOT IN ('sqlite_stat1','sqlite_stat4') ORDER BY type,name",
                             "storage.schema");
    if (!statement)
        return std::unexpected(statement.error());
    Catalog result;
    int rc;
    while ((rc = sqlite3_step(statement->get())) == SQLITE_ROW) {
        Object object{text(statement->get(), 0), text(statement->get(), 1),
                      text(statement->get(), 2), std::nullopt};
        if (sqlite3_column_type(statement->get(), 3) != SQLITE_NULL)
            object.sql = text(statement->get(), 3);
        result.push_back(std::move(object));
    }
    if (rc != SQLITE_DONE)
        return std::unexpected(sqlite_error(db, rc, "storage.schema"));
    return result;
}
Result<Catalog> reference_catalog() {
    sqlite3* raw = nullptr;
    const int rc =
        sqlite3_open_v2(":memory:", &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> reference{raw, &sqlite3_close_v2};
    if (rc != SQLITE_OK)
        return std::unexpected(sqlite_error(raw, rc, "storage.schema"));
    for (const auto* sql : version_one_sql) {
        auto status = execute(raw, sql, "storage.schema");
        if (!status)
            return std::unexpected(status.error());
    }
    return read_catalog(raw);
}
Status compare_catalogs(const Catalog& actual, const Catalog& expected) {
    std::size_t i = 0;
    while (i < actual.size() && i < expected.size() && actual[i] == expected[i]) ++i;
    if (i == actual.size() && i == expected.size())
        return {};
    if (i == actual.size())
        return std::unexpected(schema_error("Missing schema object: " + expected[i].name));
    if (i == expected.size())
        return std::unexpected(schema_error("Unexpected schema object: " + actual[i].name));
    const auto actual_key = std::tie(actual[i].type, actual[i].name);
    const auto expected_key = std::tie(expected[i].type, expected[i].name);
    if (actual_key < expected_key)
        return std::unexpected(schema_error("Unexpected schema object: " + actual[i].name));
    if (expected_key < actual_key)
        return std::unexpected(schema_error("Missing schema object: " + expected[i].name));
    return std::unexpected(schema_error("Schema definition mismatch: " + expected[i].name));
}
}  // namespace

Status ensure_schema(sqlite3* db, OpenMode mode) {
    auto transaction = Transaction::begin(
        db, mode == OpenMode::read_only ? TransactionMode::read : TransactionMode::write);
    if (!transaction)
        return std::unexpected(transaction.error());
    auto version = query_integer(db, "PRAGMA main.user_version", "storage.schema");
    if (!version)
        return std::unexpected(version.error());
    if (*version != 0 && *version != schema_version)
        return std::unexpected(
            schema_error("Unsupported database version: " + std::to_string(*version)));

    auto actual = read_catalog(db);
    if (!actual)
        return std::unexpected(actual.error());

    if (*version == 0) {
        if (!actual->empty())
            return std::unexpected(
                schema_error("Cannot initialize a non-empty unversioned database"));
        if (mode == OpenMode::read_only)
            return std::unexpected(
                schema_error("Database requires initialization with write access"));
        for (const auto* sql : version_one_sql) {
            auto created = execute(db, sql, "storage.initialize");
            if (!created)
                return created;
        }
        actual = read_catalog(db);
        if (!actual)
            return std::unexpected(actual.error());
    }

    auto expected = reference_catalog();
    if (!expected)
        return std::unexpected(expected.error());
    auto valid = compare_catalogs(*actual, *expected);
    if (!valid)
        return valid;

    if (*version == 0) {
        auto updated = execute(db, "PRAGMA main.user_version=1", "storage.initialize");
        if (!updated)
            return updated;
    }
    return transaction->commit();
}
}  // namespace mailai::store_detail

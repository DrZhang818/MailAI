#pragma once

#include <functional>
#include <utility>

namespace mailai::store_detail {

template <std::ranges::input_range Rows>
Status SqlContext::execute_many(
    std::string_view sql, std::initializer_list<SqlValue> common, Rows&& rows
) const {
    auto statement = _prepare(sql, common);
    if (!statement) return std::unexpected(statement.error());

    const auto parameter_count = static_cast<std::size_t>(sqlite3_bind_parameter_count(statement->get()));
    const auto suffix_count = parameter_count - common.size();
    const auto first_index = static_cast<int>(common.size()) + 1;

    for (const auto& values : rows) {
        const std::span<const SqlValue> parameters{values};
        if (parameters.size() != suffix_count)
            return fail(ErrorCode::storage_error, "Batch row does not match SQL parameter count");

        auto bound = _bind(*statement, first_index, parameters);
        if (!bound) return bound;

        auto executed = _execute(*statement);
        if (!executed) return executed;
    }
    return {};
}

template <class... Values>
Status SqlRow::read(Values&... values) const {
    if (static_cast<std::size_t>(sqlite3_column_count(statement_)) != sizeof...(Values))
        return fail("Unexpected SQL result column count");

    Status result;
    int index = 0;
    auto read_next = [&](auto& value) {
        if (result) result = column(index++, value);
    };
    (read_next(values), ...);
    return result;
}

template <class T>
Status SqlRow::column(int index, std::optional<T>& value) const {
    if (sqlite3_column_type(statement_, index) == SQLITE_NULL) {
        value.reset();
        return {};
    }

    T decoded{};
    auto result = column(index, decoded);
    if (!result) return result;
    value = std::move(decoded);
    return {};
}

template <class T>
Result<T> decode_scalar(const SqlRow& row) {
    T value{};
    auto decoded = row.read(value);
    if (!decoded) return std::unexpected(decoded.error());
    return value;
}

template <class Decoder>
Result<std::optional<DecodedValue<Decoder>>> SqlContext::query_one(
    std::string_view sql, std::initializer_list<SqlValue> values, Decoder decode
) const {
    auto statement = _prepare(sql, values);
    if (!statement) return std::unexpected(statement.error());

    auto found = _next(*statement);
    if (!found) return std::unexpected(found.error());
    if (!*found) return std::optional<DecodedValue<Decoder>>{};

    auto value = std::invoke(decode, SqlRow{statement->get(), *this});
    if (!value) return fail(value.error());

    auto finished = _finish(*statement);
    if (!finished) return std::unexpected(finished.error());

    return std::optional<DecodedValue<Decoder>>{std::move(*value)};
}

template <class Visitor>
Result<std::size_t> SqlContext::scan(
    std::string_view sql, std::initializer_list<SqlValue> values, Visitor visit
) const {
    auto statement = _prepare(sql, values);
    if (!statement) return std::unexpected(statement.error());

    std::size_t count = 0;
    while (true) {
        auto found = _next(*statement);
        if (!found) return std::unexpected(found.error());
        if (!*found) return count;

        auto visited = std::invoke(visit, SqlRow{statement->get(), *this});
        if (!visited) return fail(visited.error());
        ++count;
    }
}

template <class Decoder>
Result<std::vector<DecodedValue<Decoder>>> SqlContext::query_all(
    std::string_view sql, std::initializer_list<SqlValue> values, Decoder decode
) const {
    std::vector<DecodedValue<Decoder>> result;
    auto read = scan(sql, values, [&](const SqlRow& row) -> Status {
        auto value = std::invoke(decode, row);
        if (!value) return std::unexpected(value.error());
        result.push_back(std::move(*value));
        return {};
    });
    if (!read) return std::unexpected(read.error());
    return result;
}

}  // namespace mailai::store_detail

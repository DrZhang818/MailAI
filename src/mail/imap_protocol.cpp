#include "imap_protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "mailai/error_message.h"

namespace mailai::imap_detail {
namespace errors = error_message::imap;
namespace {

// 通用协议解析工具。
bool ascii_iequal(std::string_view left, std::string_view right) {
    const auto upper = [](char ch) {
        return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
    };
    return std::ranges::equal(
        left, right, [upper](char a, char b) { return upper(a) == upper(b); });
}

std::string_view trim_left(std::string_view text) {
    const auto start = text.find_first_not_of(" \t");
    return start == std::string_view::npos ? std::string_view{} : text.substr(start);
}

std::string_view take_word(std::string_view& text) {
    text = trim_left(text);
    if (text.empty()) {
        return {};
    }
    const auto word = text.substr(0, text.find_first_of(" \t"));
    text.remove_prefix(word.size());
    return word;
}

std::optional<std::uint32_t> parse_u32(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint32_t value{};
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint32_t> parse_nonzero_u32(std::string_view text) {
    const auto value = parse_u32(text);
    if (!value || *value == 0) {
        return std::nullopt;
    }
    return value;
}

const char* update_uid_validity(std::string_view line, std::uint32_t& current) {
    line = trim_left(line);
    if (!line.starts_with('[')) {
        return nullptr;
    }

    const auto closing = line.find(']');
    auto code = line.substr(1, closing == std::string_view::npos ? closing : closing - 1);
    if (!ascii_iequal(take_word(code), "UIDVALIDITY")) {
        return nullptr;
    }
    if (closing == std::string_view::npos ||
        (closing + 1 < line.size() && line[closing + 1] != ' ' && line[closing + 1] != '\t')) {
        return errors::invalid_uid_validity;
    }
    const auto validity = parse_nonzero_u32(take_word(code));
    if (!validity || !take_word(code).empty()) {
        return errors::invalid_uid_validity;
    }
    if (current != 0 && current != *validity) {
        return errors::conflicting_uid_validity;
    }
    current = *validity;
    return nullptr;
}

const char* check_fetch_validity(
    std::string_view ok_text, std::uint32_t expected, std::uint32_t& observed) {
    std::uint32_t reported = 0;
    if (const auto* message = update_uid_validity(ok_text, reported)) {
        return message;
    }
    if (reported != 0) {
        if (reported != expected) {
            return errors::uid_validity_changed;
        }
        observed = reported;
    }
    return nullptr;
}

ErrorCode fetch_response_error_code(const char* message) {
    if (message == errors::uid_validity_changed) {
        return ErrorCode::remote_gone;
    }
    if (message == errors::service_unavailable) {
        return ErrorCode::network_error;
    }
    return ErrorCode::protocol_error;
}

constexpr std::size_t max_raw_bytes = 20 * 1024 * 1024;
constexpr std::size_t max_fetch_response_bytes = max_raw_bytes + 256 * 1024;

std::string_view strip_line_end(std::string_view line) {
    if (line.ends_with('\n'))
        line.remove_suffix(1);
    if (line.ends_with('\r'))
        line.remove_suffix(1);
    return line;
}

std::optional<std::string_view> take_response_line(std::string_view& response) {
    const auto end = response.find('\n');
    if (end == std::string_view::npos)
        return std::nullopt;
    const auto line = strip_line_end(response.substr(0, end + 1));
    response.remove_prefix(end + 1);
    return line;
}

// 只有明确的临时不可用响应才重试普通命令拒绝错误。
bool is_service_unavailable(std::string_view line) {
    const auto tag = take_word(line);
    const auto status = take_word(line);
    const bool rejected = !tag.empty() && tag != "*" && tag != "+" && ascii_iequal(status, "NO");
    const bool disconnected = tag == "*" && ascii_iequal(status, "BYE");
    if (!rejected && !disconnected) {
        return false;
    }
    line = trim_left(line);
    constexpr std::string_view code = "[UNAVAILABLE]";
    return ascii_iequal(line.substr(0, code.size()), code) &&
           (line.size() == code.size() || line[code.size()] == ' ' || line[code.size()] == '\t');
}

// 单封邮件的属性及原文解析。
// 提取未标记 FETCH 响应的属性列表，忽略其他响应。
std::optional<std::string_view> fetch_list(std::string_view line) {
    const auto tag = take_word(line);
    const auto sequence = take_word(line);
    if (tag != "*" || !ascii_iequal(take_word(line), "FETCH")) {
        return std::nullopt;
    }
    line = trim_left(line);
    if (!parse_nonzero_u32(sequence) || !line.starts_with('(')) {
        return std::string_view{};
    }
    line.remove_prefix(1);
    return line;
}

std::string_view take_fetch_token(std::string_view& text) {
    text = trim_left(text);
    const auto token = text.substr(0, text.find_first_of(" \t()"));
    text.remove_prefix(token.size());
    return token;
}

struct FetchFields {
    std::optional<std::uint32_t> uid;
    std::optional<std::string_view> date;
    std::optional<std::size_t> literal_size;
};

enum class FetchListEnd { invalid, literal, closed };

FetchListEnd read_fetch_fields(std::string_view text, FetchFields& fields) {
    while (!(text = trim_left(text)).empty()) {
        if (text.front() == ')') {
            text.remove_prefix(1);
            return trim_left(text).empty() ? FetchListEnd::closed : FetchListEnd::invalid;
        }

        const auto name = take_fetch_token(text);
        text = trim_left(text);
        if (ascii_iequal(name, "UID")) {
            if (fields.uid)
                return FetchListEnd::invalid;
            fields.uid = parse_nonzero_u32(take_fetch_token(text));
            if (!fields.uid)
                return FetchListEnd::invalid;
        } else if (ascii_iequal(name, "INTERNALDATE")) {
            if (fields.date || !text.starts_with('"'))
                return FetchListEnd::invalid;
            const auto closing = text.find('"', 1);
            if (closing == std::string_view::npos)
                return FetchListEnd::invalid;
            fields.date = text.substr(1, closing - 1);
            text.remove_prefix(closing + 1);
        } else if (ascii_iequal(name, "BODY[]")) {
            const auto literal = take_fetch_token(text);
            if (fields.literal_size || !literal.starts_with('{') || !literal.ends_with('}') ||
                literal.size() < 3 || !trim_left(text).empty()) {
                return FetchListEnd::invalid;
            }
            fields.literal_size = parse_u32(literal.substr(1, literal.size() - 2));
            return fields.literal_size ? FetchListEnd::literal : FetchListEnd::invalid;
        } else if (ascii_iequal(name, "FLAGS") || ascii_iequal(name, "MODSEQ")) {
            if (!text.starts_with('('))
                return FetchListEnd::invalid;
            const auto closing = text.find(')');
            if (closing == std::string_view::npos)
                return FetchListEnd::invalid;
            text.remove_prefix(closing + 1);
        } else if (ascii_iequal(name, "RFC822.SIZE")) {
            if (!parse_u32(take_fetch_token(text)))
                return FetchListEnd::invalid;
        } else {
            return FetchListEnd::invalid;
        }
        if (!text.empty() && text.front() != ' ' && text.front() != '\t' && text.front() != ')') {
            return FetchListEnd::invalid;
        }
    }
    return FetchListEnd::invalid;
}

// 从原文结束的位置继续解析 FETCH 属性和协议响应，检查命令结果及邮件标识。
std::expected<FetchFields, const char*> finish_fetch_fields(std::string_view response,
    FetchFields fields,
    std::uint32_t expected_validity,
    std::uint32_t& observed_validity) {
    const auto suffix = take_response_line(response);
    if (!suffix || read_fetch_fields(*suffix, fields) != FetchListEnd::closed)
        return std::unexpected(errors::invalid_fetch_response);
    while (const auto line = take_response_line(response)) {
        if (is_service_unavailable(*line)) {
            return std::unexpected(errors::service_unavailable);
        }
        if (const auto list = fetch_list(*line)) {
            FetchFields extra;
            if (read_fetch_fields(*list, extra) != FetchListEnd::closed)
                return std::unexpected(errors::invalid_fetch_response);
        } else {
            auto rest = *line;
            const auto tag = take_word(rest);
            const auto kind = take_word(rest);
            const bool ok = ascii_iequal(kind, "OK");
            if (!tag.empty() && tag != "+" && ok) {
                if (const auto* message =
                        check_fetch_validity(rest, expected_validity, observed_validity)) {
                    return std::unexpected(message);
                }
            }
            if (!tag.empty() && tag != "*" && tag != "+") {
                if (ok && response.empty()) {
                    return fields;
                }
                break;
            }
        }
    }
    return std::unexpected(errors::invalid_fetch_response);
}

struct FetchLiteral {
    FetchFields fields;
    std::string_view body;
    std::string_view overflow;
    std::string_view response;
};

std::optional<FetchLiteral> separate_fetch_literal(std::string_view headers,
    std::string_view payload,
    std::optional<std::size_t> prefix_offset,
    std::size_t literal_offset,
    std::size_t literal_size) {
    if (!prefix_offset)
        return std::nullopt;
    const auto prefix = headers.substr(*prefix_offset, literal_offset - *prefix_offset);
    const auto list = fetch_list(strip_line_end(prefix));
    FetchFields fields;
    if (!list || read_fetch_fields(*list, fields) != FetchListEnd::literal)
        return std::nullopt;
    const auto remainder = headers.substr(literal_offset);

    // 定位原文前缀，检查下载数据是否超出声明的字节数。
    const auto start = payload.find(prefix);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto body = payload.substr(start + prefix.size());
    if (body.size() > literal_size) {
        return std::nullopt;
    }

    // 收到额外的未标记响应时，剩余原文可能通过 HEADERFUNCTION 返回。
    const auto missing = literal_size - body.size();
    if (remainder.size() < missing) {
        return std::nullopt;
    }
    return FetchLiteral{fields, body, remainder.substr(0, missing), remainder.substr(missing)};
}

}  // namespace

FetchTransfer::FetchTransfer(const RemoteKey& remote_key, std::uint32_t inherited)
    : key(remote_key), inherited_validity(inherited == remote_key.uid_validity ? inherited : 0) {}

bool FetchTransfer::fail(const char* message, ErrorCode code) noexcept {
    error = message;
    error_code = code;
    return false;
}

bool FetchTransfer::append(std::string& buffer, std::string_view data) {
    if (data.size() > max_fetch_response_bytes - buffer.size()) {
        return fail(errors::fetch_response_too_large);
    }
    buffer.append(data);
    return true;
}

bool FetchTransfer::append_header(std::string_view data) {
    if (error)
        return false;
    // 原文前缀之后可能收到剩余的邮件字节，须分离完整原文后再解析协议字段。
    if (prefix_offset)
        return append(headers, data);

    const auto line = strip_line_end(data);
    if (is_service_unavailable(line)) {
        return fail(errors::service_unavailable, ErrorCode::network_error);
    }
    auto rest = line;
    const auto tag = take_word(rest);
    const auto kind = take_word(rest);
    if (!tag.empty() && tag != "+" && ascii_iequal(kind, "OK")) {
        if (const auto* message = check_fetch_validity(rest, key.uid_validity, observed_validity)) {
            return fail(message, fetch_response_error_code(message));
        }
        const auto code = take_word(rest);
        if (tag != "*" && !observed_validity &&
            (ascii_iequal(code, "[READ-ONLY]") || ascii_iequal(code, "[READ-WRITE]"))) {
            return fail(errors::missing_uid_validity);
        }
    }

    if (const auto list = fetch_list(line)) {
        if (!observed_validity && inherited_validity != key.uid_validity)
            return fail(errors::missing_uid_validity);
        FetchFields fields;
        switch (read_fetch_fields(*list, fields)) {
            case FetchListEnd::invalid:
                return fail(errors::invalid_fetch_response);
            case FetchListEnd::closed:
                target_without_body |= fields.uid == key.uid || fields.date.has_value();
                break;
            case FetchListEnd::literal:
                literal_size = *fields.literal_size;
                if (literal_size > max_raw_bytes)
                    return fail(errors::fetch_too_large);
                prefix_offset = headers.size();
                literal_offset = headers.size() + data.size();
                break;
        }
    }
    return append(headers, data);
}

bool FetchTransfer::append_body(std::string_view data) {
    return !error && append(payload, data);
}

void FetchTransfer::abort() noexcept {
    fail(errors::invalid_fetch_response);
}

Result<FetchedMail> FetchTransfer::finish(
    std::optional<Error> transport_error, bool require_observed_validity) const {
    auto failure = [&](ErrorCode code, const char* message) {
        return std::unexpected(Error{
            .code = code,
            .message = message,
            .account_id = key.account_id,
            .mail_id = std::nullopt,
            .stage = errors::fetch_stage
        });
    };

    if (error) {
        return failure(error_code, error);
    }
    const auto literal =
        separate_fetch_literal(headers, payload, prefix_offset, literal_offset, literal_size);
    auto validity = observed_validity;
    std::expected<FetchFields, const char*> fields =
        std::unexpected(errors::invalid_fetch_response);
    if (literal) {
        fields =
            finish_fetch_fields(literal->response, literal->fields, key.uid_validity, validity);
        if (!fields && fields.error() != errors::invalid_fetch_response) {
            return failure(fetch_response_error_code(fields.error()), fields.error());
        }
    }
    if (transport_error) {
        return std::unexpected(std::move(*transport_error));
    }
    const bool has_validity =
        validity != 0 || (!require_observed_validity && inherited_validity == key.uid_validity);
    if (!has_validity) {
        return failure(ErrorCode::protocol_error, errors::missing_uid_validity);
    }
    if (!prefix_offset) {
        return target_without_body
                   ? failure(ErrorCode::protocol_error, errors::invalid_fetch_response)
                   : failure(ErrorCode::remote_gone, errors::remote_mail_gone);
    }
    if (!fields || fields->uid != key.uid || !fields->date) {
        return failure(ErrorCode::protocol_error, errors::invalid_fetch_response);
    }
    const auto received_at = parse_internal_date(*fields->date, key.account_id);
    if (!received_at) {
        return std::unexpected(received_at.error());
    }

    std::string raw;
    raw.reserve(literal_size);
    raw.append(literal->body);
    raw.append(literal->overflow);
    return FetchedMail{.key = key, .received_at_utc = *received_at, .raw = std::move(raw)};
}

Result<std::int64_t> parse_internal_date(std::string_view date, const AccountId& account_id) {
    namespace chrono = std::chrono;

    auto fail = [&] {
        return std::unexpected(Error{
            .code = ErrorCode::protocol_error,
            .message = errors::invalid_internal_date,
            .account_id = account_id,
            .mail_id = std::nullopt,
            .stage = errors::fetch_stage
        });
    };

    // 1. 检查 INTERNALDATE 的固定格式：DD-Mon-YYYY HH:MM:SS +HHMM。
    if (date.size() != 26 || date[2] != '-' || date[6] != '-' || date[11] != ' ' ||
        date[14] != ':' || date[17] != ':' || date[20] != ' ' ||
        (date[21] != '+' && date[21] != '-')) {
        return fail();
    }

    // 2. 解析年月日、时分秒和时区偏移。
    const auto day = parse_u32(date[0] == ' ' ? date.substr(1, 1) : date.substr(0, 2));
    const auto year = parse_u32(date.substr(7, 4));
    const auto hour = parse_u32(date.substr(12, 2));
    const auto minute = parse_u32(date.substr(15, 2));
    const auto second = parse_u32(date.substr(18, 2));
    const auto zone_hour = parse_u32(date.substr(22, 2));
    const auto zone_minute = parse_u32(date.substr(24, 2));

    constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    const auto month = std::ranges::find_if(
        months, [&](std::string_view name) { return ascii_iequal(name, date.substr(3, 3)); });

    if (!day || !year || !hour || !minute || !second || !zone_hour || !zone_minute ||
        month == months.end()) {
        return fail();
    }

    // 3. 验证真实日历日期、时间及偏移范围。
    const chrono::year_month_day calendar_date{
        chrono::year{static_cast<int>(*year)},
        chrono::month{static_cast<unsigned>(month - months.begin() + 1)},
        chrono::day{*day}
    };
    if (*year == 0 || !calendar_date.ok() || *hour > 23 || *minute > 59 || *second > 59 ||
        *zone_hour > 23 || *zone_minute > 59) {
        return fail();
    }

    const auto local = chrono::sys_days{calendar_date} + chrono::hours{*hour} +
                       chrono::minutes{*minute} + chrono::seconds{*second};

    // 4. 减去带符号的时区偏移，转换为 UTC。
    auto offset = chrono::hours{*zone_hour} + chrono::minutes{*zone_minute};
    if (date[21] == '-') {
        offset = -offset;
    }
    const auto utc = local - offset;

    // 5. 返回有符号的 Unix 时间戳（秒）。
    return utc.time_since_epoch().count();
}

// 收件箱快照及临时服务故障的解析入口。
Result<MailboxSnapshot> parse_mailbox_snapshot(
    std::string_view response, const AccountId& account_id) {
    auto fail = [&](const char* message) {
        return std::unexpected(Error{
            .code = ErrorCode::protocol_error,
            .message = message,
            .account_id = account_id,
            .mail_id = std::nullopt,
            .stage = errors::snapshot_stage
        });
    };

    MailboxSnapshot snapshot;
    bool has_search = false;

    // 1. 按行读取协议响应。
    while (!response.empty()) {
        const auto end = response.find('\n');
        auto line = response.substr(0, end);
        response = end == std::string_view::npos ? std::string_view{} : response.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        const auto tag = take_word(line);
        const auto kind = take_word(line);

        // 2. 提取 OK 响应代码中的 UIDVALIDITY。
        if (!tag.empty() && tag != "+" && ascii_iequal(kind, "OK")) {
            if (const auto* error = update_uid_validity(line, snapshot.uid_validity)) {
                return fail(error);
            }
        } else if (tag == "*" && ascii_iequal(kind, "SEARCH")) {
            // 3. 提取未标记 SEARCH 响应中的所有 UID。
            has_search = true;
            for (auto word = take_word(line); !word.empty(); word = take_word(line)) {
                const auto uid = parse_nonzero_u32(word);
                if (!uid) {
                    return fail(errors::invalid_search_uid);
                }
                snapshot.uids.push_back(*uid);
            }
        }
    }

    // 4. 检查必需响应，排序并去重。
    if (snapshot.uid_validity == 0) {
        return fail(errors::missing_uid_validity);
    }
    if (!has_search) {
        return fail(errors::missing_search);
    }
    std::sort(snapshot.uids.begin(), snapshot.uids.end());
    snapshot.uids.erase(
        std::unique(snapshot.uids.begin(), snapshot.uids.end()), snapshot.uids.end());

    // 5. 返回收件箱快照。
    return snapshot;
}

bool has_service_unavailable(std::string_view response) {
    while (const auto line = take_response_line(response)) {
        if (is_service_unavailable(*line)) {
            return true;
        }
    }
    return false;
}

}  // namespace mailai::imap_detail

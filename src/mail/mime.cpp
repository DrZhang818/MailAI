#include "mailai/mime.h"

#include <gmime/gmime.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "mailai/error_message.h"

namespace mailai {
namespace {

constexpr std::size_t max_raw_bytes = 20 * 1024 * 1024;
constexpr std::size_t max_mime_depth = 32;
constexpr std::size_t max_mime_parts = 256;
constexpr std::size_t max_table_columns = 128;
constexpr std::size_t max_table_cells = 100000;
constexpr std::size_t max_repeated_table_text_bytes = max_raw_bytes;

struct GObjectUnref {
    void operator()(void* object) const noexcept {
        g_object_unref(object);
    }
};

template <typename T>
using GMimePtr = std::unique_ptr<T, GObjectUnref>;

std::unexpected<Error> parse_failure(const char* message) {
    return std::unexpected(Error{
        .code = ErrorCode::parse_error,
        .message = message,
        .account_id = std::nullopt,
        .mail_id = std::nullopt,
        .stage = "mime.parse"
    });
}

Status validate_mime_structure(GMimeObject* part, std::size_t depth, std::size_t& count) {
    // 1. 检查部件和嵌套深度
    if (part == nullptr) {
        return parse_failure(error_message::mime::missing_part);
    }
    if (depth > max_mime_depth) {
        return parse_failure(error_message::mime::depth_limit);
    }

    // 2. 检查部件总数
    if (++count > max_mime_parts) {
        return parse_failure(error_message::mime::part_count_limit);
    }

    // 3. 检查 multipart 的所有子部件
    if (GMIME_IS_MULTIPART(part)) {
        GMimeMultipart* multipart = GMIME_MULTIPART(part);
        const int child_count = g_mime_multipart_get_count(multipart);

        for (int i = 0; i < child_count; ++i) {
            GMimeObject* child = g_mime_multipart_get_part(multipart, i);
            auto status = validate_mime_structure(child, depth + 1, count);
            if (!status) {
                return status;
            }
        }
    } else if (GMIME_IS_MESSAGE_PART(part)) {
        // 4. 检查嵌入邮件的根部件
        GMimeMessage* embedded = g_mime_message_part_get_message(GMIME_MESSAGE_PART(part));
        if (embedded == nullptr) {
            return parse_failure(error_message::mime::missing_embedded_message);
        }
        return validate_mime_structure(g_mime_message_get_mime_part(embedded), depth + 1, count);
    }

    return {};
}

Result<GMimePtr<GMimeMessage>> parse_mime_message(std::string_view raw) {
    if (raw.empty()) {
        return parse_failure(error_message::mime::empty_message);
    }
    if (raw.size() > max_raw_bytes) {
        return parse_failure(error_message::mime::message_size_limit);
    }

    static std::once_flag gmime_init_flag;
    std::call_once(gmime_init_flag, g_mime_init);

    GMimePtr<GMimeStream> stream{g_mime_stream_mem_new_with_buffer(raw.data(), raw.size())};
    GMimePtr<GMimeParser> parser{g_mime_parser_new_with_stream(stream.get())};
    GMimePtr<GMimeMessage> message{g_mime_parser_construct_message(parser.get(), nullptr)};
    if (!message) {
        return parse_failure(error_message::mime::message_parse_failed);
    }

    std::size_t count = 0;
    if (auto status =
            validate_mime_structure(g_mime_message_get_mime_part(message.get()), 1, count);
        !status) {
        return std::unexpected(status.error());
    }
    return message;
}

GMimeTextPart* as_text_part(GMimeObject* part, const char* subtype) {
    if (part == nullptr || !GMIME_IS_TEXT_PART(part)) {
        return nullptr;
    }

    GMimeContentType* type = g_mime_object_get_content_type(part);
    if (!g_mime_content_type_is_type(type, "text", subtype)) {
        return nullptr;
    }

    return GMIME_TEXT_PART(part);
}

bool is_attachment(GMimeObject* part) {
    GMimeContentDisposition* disposition = g_mime_object_get_content_disposition(part);
    if (disposition != nullptr) {
        return g_mime_content_disposition_is_attachment(disposition);
    }
    if (!GMIME_IS_PART(part)) {
        return false;
    }

    const char* filename = g_mime_part_get_filename(GMIME_PART(part));
    return filename != nullptr && filename[0] != '\0';
}

std::vector<GMimeTextPart*> select_text_parts(GMimeObject* part, const char* subtype) {
    // 1. 判断当前部件
    if (part == nullptr || is_attachment(part)) {
        return {};
    }
    if (GMimeTextPart* text_part = as_text_part(part, subtype)) {
        return {text_part};
    }
    if (!GMIME_IS_MULTIPART(part)) {
        return {};
    }

    // 2. 决定遍历顺序
    GMimeMultipart* multipart = GMIME_MULTIPART(part);
    const int count = g_mime_multipart_get_count(multipart);
    GMimeContentType* type = g_mime_object_get_content_type(part);
    const bool is_alternative = g_mime_content_type_is_type(type, "multipart", "alternative");
    const bool is_mixed = g_mime_content_type_is_type(type, "multipart", "mixed");

    // 3. 递归处理子部件结果
    std::vector<GMimeTextPart*> text_parts;
    for (int i = 0; i < count; ++i) {
        const int index = is_alternative ? count - 1 - i : i;
        GMimeObject* child = g_mime_multipart_get_part(multipart, index);

        auto child_parts = select_text_parts(child, subtype);
        if (child_parts.empty()) {
            continue;
        }

        if (!is_mixed) {
            return child_parts;
        }

        text_parts.insert(text_parts.end(), child_parts.begin(), child_parts.end());
    }

    return text_parts;
}

Result<std::string> decode_text_part(GMimeTextPart* part) {
    // 1. 读取声明的字符集
    if (const char* charset = g_mime_text_part_get_charset(part)) {
        if (charset[0] == '\0') {
            return parse_failure(error_message::mime::unsupported_charset);
        }

        // 2. 检查是否支持转化为 UTF-8
        GMimePtr<GMimeFilter> filter{g_mime_filter_charset_new(charset, "UTF-8")};
        if (!filter) {
            return parse_failure(error_message::mime::unsupported_charset);
        }
    }

    // 3. 读取解码后的文本并校验 UTF-8
    std::unique_ptr<char, decltype(&g_free)> text{g_mime_text_part_get_text(part), &g_free};
    if (!text || !g_utf8_validate(text.get(), -1, nullptr)) {
        return parse_failure(error_message::mime::invalid_utf8_body);
    }

    // 4. 复制文本并返回
    return std::string{text.get()};
}

// 延迟输出空格和块级换行，避免缩进和嵌套标签引入多余的空白。
enum class HtmlSeparator { none, space, line_break };

struct PresentationBudget {
    std::size_t cells{max_table_cells};
    std::size_t repeated_bytes{max_repeated_table_text_bytes};
};

struct HtmlPresentation {
    std::vector<BodyBlock> blocks;
    PresentationBudget& budget;
    bool has_table{};
};

bool append_structured_table(const xmlNode* table,
    std::string& text,
    HtmlSeparator& separator,
    HtmlPresentation& presentation);

bool is_html_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

std::string_view html_element_name(const xmlNode* node) {
    if (node->type != XML_ELEMENT_NODE) {
        return {};
    }
    return reinterpret_cast<const char*>(node->name);
}

bool ignore_html_element(std::string_view name) {
    return name == "head" || name == "script" || name == "style" || name == "template";
}

void flush_html_separator(std::string& text, HtmlSeparator& separator) {
    if (!text.empty()) {
        if (separator == HtmlSeparator::line_break && text.back() != '\n') {
            text.push_back('\n');
        } else if (separator == HtmlSeparator::space && !is_html_whitespace(text.back())) {
            text.push_back(' ');
        }
    }
    separator = HtmlSeparator::none;
}

void append_collapsed_html_text(
    std::string_view content, std::string& text, HtmlSeparator& separator) {
    for (char ch : content) {
        if (is_html_whitespace(ch)) {
            if (separator == HtmlSeparator::none) {
                separator = HtmlSeparator::space;
            }
            continue;
        }

        flush_html_separator(text, separator);
        text.push_back(ch);
    }
}

void append_html_text(const xmlNode* node,
    std::string& text,
    HtmlSeparator& separator,
    bool preserve_whitespace = false,
    HtmlPresentation* presentation = nullptr) {
    bool previous_cell = false;
    for (; node != nullptr; node = node->next) {
        if (node->type == XML_TEXT_NODE) {
            if (node->content != nullptr) {
                const std::string_view content{reinterpret_cast<const char*>(node->content)};
                if (preserve_whitespace) {
                    flush_html_separator(text, separator);
                    text += content;
                } else {
                    append_collapsed_html_text(content, text, separator);
                }
            }
            continue;
        }
        if (node->type != XML_ELEMENT_NODE) {
            continue;
        }

        const auto name = html_element_name(node);
        if (ignore_html_element(name)) {
            continue;
        }
        if (name == "table" && presentation &&
            append_structured_table(node, text, separator, *presentation)) {
            continue;
        }

        const bool is_cell = name == "td" || name == "th";
        if (is_cell && previous_cell) {
            separator = HtmlSeparator::none;
            text.push_back('\t');
        }
        previous_cell = is_cell;

        if (name == "br") {
            if (separator == HtmlSeparator::line_break) {
                flush_html_separator(text, separator);
            }
            separator = HtmlSeparator::none;
            text.push_back('\n');
            continue;
        }

        const bool is_heading =
            name.size() == 2 && name.front() == 'h' && name.back() >= '1' && name.back() <= '6';
        const bool is_block =
            !preserve_whitespace && (name == "p" || name == "div" || name == "li" || name == "tr" ||
                                        name == "blockquote" || name == "pre" || is_heading);

        // 让单元格中的首个块仍位于当前列。
        if (is_block && (text.empty() || text.back() != '\t')) {
            separator = HtmlSeparator::line_break;
        }
        // 先结束上一行，避免本行首个单元格为空时丢失换行。
        if (name == "tr") {
            flush_html_separator(text, separator);
        }

        append_html_text(
            node->children, text, separator, preserve_whitespace || name == "pre", presentation);

        if (is_block) {
            separator = HtmlSeparator::line_break;
        }
    }
}

using HtmlDocument = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

Result<HtmlDocument> parse_html_document(std::string_view html) {
    if (html.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return parse_failure(error_message::mime::html_too_large);
    }

    const int options = HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING;
    HtmlDocument document{
        htmlReadMemory(html.data(), static_cast<int>(html.size()), nullptr, "UTF-8", options),
        &xmlFreeDoc
    };
    if (!document) {
        return parse_failure(error_message::mime::html_parse_failed);
    }

    if (xmlDocGetRootElement(document.get()) == nullptr) {
        return parse_failure(error_message::mime::missing_html_root);
    }

    return document;
}

Result<std::string> html_to_text(std::string_view html) {
    if (html.empty()) {
        return std::string{};
    }

    auto document = parse_html_document(html);
    if (!document) {
        return std::unexpected(document.error());
    }

    std::string text;
    HtmlSeparator separator = HtmlSeparator::none;
    append_html_text(xmlDocGetRootElement(document->get()), text, separator);
    if (separator == HtmlSeparator::line_break) {
        flush_html_separator(text, separator);
    }

    return text;
}

bool has_visible_text(std::string_view text) {
    for (const char* ch = text.data(); ch != text.data() + text.size(); ch = g_utf8_next_char(ch)) {
        if (!g_unichar_isspace(g_utf8_get_char(ch))) {
            return true;
        }
    }
    return false;
}

void flush_body_text(std::string& text, HtmlSeparator& separator, std::vector<BodyBlock>& blocks) {
    if (has_visible_text(text)) {
        blocks.emplace_back(std::move(text));
    }
    text.clear();
    separator = HtmlSeparator::none;
}

bool contains_nested_table(const xmlNode* node) {
    for (; node != nullptr; node = node->next) {
        const auto name = html_element_name(node);
        if (ignore_html_element(name)) {
            continue;
        }
        if (name == "table" || contains_nested_table(node->children)) {
            return true;
        }
    }
    return false;
}

struct XmlCharFree {
    void operator()(xmlChar* value) const noexcept {
        xmlFree(value);
    }
};

std::size_t read_span(const xmlNode* cell, const char* property, std::size_t max, bool allow_zero) {
    const std::unique_ptr<xmlChar, XmlCharFree> value{xmlGetProp(
        cell, reinterpret_cast<const xmlChar*>(property))};
    if (!value) {
        return 1;
    }
    std::string_view chars{reinterpret_cast<const char*>(value.get())};
    while (!chars.empty() && is_html_whitespace(chars.front())) {
        chars.remove_prefix(1);
    }
    while (!chars.empty() && is_html_whitespace(chars.back())) {
        chars.remove_suffix(1);
    }
    std::size_t span{};
    const auto [end, error] = std::from_chars(chars.data(), chars.data() + chars.size(), span);
    if (error != std::errc{} || end != chars.data() + chars.size() || span > max ||
        (!allow_zero && span == 0)) {
        return 1;
    }
    return span;
}

using HtmlRows = std::vector<const xmlNode*>;

std::vector<HtmlRows> collect_row_groups(const xmlNode* table) {
    std::vector<HtmlRows> groups;
    bool direct_rows = false;
    for (const xmlNode* child = table->children; child != nullptr; child = child->next) {
        const auto name = html_element_name(child);
        if (name == "tr") {
            if (!direct_rows) {
                groups.emplace_back();
                direct_rows = true;
            }
            groups.back().push_back(child);
        } else if (name == "thead" || name == "tbody" || name == "tfoot") {
            direct_rows = false;
            HtmlRows rows;
            for (const xmlNode* row = child->children; row != nullptr; row = row->next) {
                if (html_element_name(row) == "tr") {
                    rows.push_back(row);
                }
            }
            if (!rows.empty()) {
                groups.push_back(std::move(rows));
            }
        }
    }
    return groups;
}

struct TableSpan {
    std::size_t until{};
    std::size_t source_row{};
    bool leading_column{};
};

std::optional<BodyTable> extract_body_table(const xmlNode* table, PresentationBudget& budget) {
    const auto groups = collect_row_groups(table);
    std::size_t row_count = 0;
    for (const auto& group : groups) {
        row_count += group.size();
    }
    if (row_count == 0 || row_count > budget.cells) {
        return std::nullopt;
    }

    const std::size_t column_limit = std::min(max_table_columns, budget.cells / row_count);
    std::size_t width = 0;
    std::size_t repeated_bytes = 0;
    bool has_content = false;
    BodyTable result;
    result.rows.reserve(row_count);

    for (const auto& group : groups) {
        std::array<TableSpan, max_table_columns> spans{};
        for (std::size_t row_index = 0; row_index < group.size(); ++row_index) {
            std::vector<std::string> row;
            // 保留当前行中已有的跨行单元格，并重复其文字以保留对应关系。
            for (std::size_t column = 0; column < width; ++column) {
                if (spans[column].until <= row_index) {
                    continue;
                }
                row.resize(column + 1);
                if (spans[column].leading_column) {
                    const auto& source = result.rows[spans[column].source_row][column];
                    if (source.size() > budget.repeated_bytes - repeated_bytes) {
                        return std::nullopt;
                    }
                    repeated_bytes += source.size();
                    row[column] = source;
                }
            }

            std::size_t column = 0;
            for (const xmlNode* cell = group[row_index]->children; cell != nullptr;
                cell = cell->next) {
                const auto name = html_element_name(cell);
                if (name != "td" && name != "th") {
                    continue;
                }

                const auto colspan = read_span(cell, "colspan", max_table_columns, false);
                auto rowspan = read_span(cell, "rowspan", 65534, true);
                const std::size_t remaining_rows = group.size() - row_index;
                rowspan = rowspan == 0 ? remaining_rows : std::min(rowspan, remaining_rows);

                // 查找足够宽的空闲区间，不覆盖来自上一行的单元格。
                while (column + colspan <= column_limit) {
                    const auto first = spans.begin() + static_cast<std::ptrdiff_t>(column);
                    const auto last = first + static_cast<std::ptrdiff_t>(colspan);
                    const auto overlap = std::find_if(first, last, [row_index](const auto& span) {
                        return span.until > row_index;
                    });
                    if (overlap == last) {
                        break;
                    }
                    column = static_cast<std::size_t>(overlap - spans.begin()) + 1;
                }
                if (column + colspan > column_limit) {
                    return std::nullopt;
                }

                row.resize(std::max(row.size(), column + colspan));
                HtmlSeparator separator = HtmlSeparator::none;
                append_html_text(cell->children, row[column], separator);
                has_content = has_content || has_visible_text(row[column]);
                for (std::size_t offset = 0; offset < colspan; ++offset) {
                    spans[column + offset] = {row_index + rowspan, result.rows.size(), offset == 0};
                }
                column += colspan;
            }
            width = std::max(width, row.size());
            result.rows.push_back(std::move(row));
        }
    }

    if (!has_content) {
        return std::nullopt;
    }
    for (auto& row : result.rows) {
        row.resize(width);
    }
    budget.cells -= row_count * width;
    budget.repeated_bytes -= repeated_bytes;
    return result;
}

bool append_structured_table(const xmlNode* table,
    std::string& text,
    HtmlSeparator& separator,
    HtmlPresentation& presentation) {
    // 邮件常用外层表格排版。递归提取内层表格，避免将它们压扁成单元格文本。
    if (contains_nested_table(table->children)) {
        return false;
    }
    auto structured = extract_body_table(table, presentation.budget);
    if (!structured) {
        return false;
    }

    for (const xmlNode* child = table->children; child != nullptr; child = child->next) {
        if (html_element_name(child) == "caption") {
            separator = HtmlSeparator::line_break;
            append_html_text(child->children, text, separator);
        }
    }
    flush_body_text(text, separator, presentation.blocks);
    presentation.blocks.emplace_back(std::move(*structured));
    presentation.has_table = true;
    return true;
}

struct BodyCandidate {
    std::vector<BodyBlock> blocks;
    bool has_body{};
    bool has_plain{};
    bool has_table{};
};

Result<BodyCandidate> read_html_candidate(GMimeTextPart* part, PresentationBudget& budget) {
    auto html = decode_text_part(part);
    if (!html) {
        return std::unexpected(html.error());
    }
    BodyCandidate candidate{.has_body = true};
    if (html->empty()) {
        return candidate;
    }
    auto document = parse_html_document(*html);
    if (!document) {
        return std::unexpected(document.error());
    }

    HtmlPresentation presentation{.budget = budget};
    std::string text;
    HtmlSeparator separator = HtmlSeparator::none;
    append_html_text(xmlDocGetRootElement(document->get()), text, separator, false, &presentation);
    flush_body_text(text, separator, presentation.blocks);
    candidate.blocks = std::move(presentation.blocks);
    candidate.has_table = presentation.has_table;
    return candidate;
}

Result<BodyCandidate> select_body_candidate(GMimeObject* part, PresentationBudget& budget) {
    if (part == nullptr || is_attachment(part)) {
        return BodyCandidate{};
    }
    if (auto* plain = as_text_part(part, "plain")) {
        auto decoded = decode_text_part(plain);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        return BodyCandidate{.blocks = {std::move(*decoded)}, .has_body = true, .has_plain = true};
    }
    if (auto* html = as_text_part(part, "html")) {
        return read_html_candidate(html, budget);
    }
    if (!GMIME_IS_MULTIPART(part)) {
        return BodyCandidate{};
    }

    auto* multipart = GMIME_MULTIPART(part);
    auto* type = g_mime_object_get_content_type(part);
    const int count = g_mime_multipart_get_count(multipart);
    const bool is_alternative = g_mime_content_type_is_type(type, "multipart", "alternative");
    const bool is_mixed = g_mime_content_type_is_type(type, "multipart", "mixed");
    BodyCandidate candidate;
    std::optional<Error> first_error;
    for (int i = 0; i < count; ++i) {
        const int index = is_alternative ? count - 1 - i : i;
        auto child_budget = budget;
        auto child =
            select_body_candidate(g_mime_multipart_get_part(multipart, index), child_budget);
        if (!child) {
            if (!is_alternative) {
                return std::unexpected(child.error());
            }
            if (!first_error) {
                first_error = child.error();
            }
            continue;
        }
        if (!child->has_body) {
            continue;
        }
        if (is_alternative) {
            // 展示有表格的 HTML 版本，否则仍优先使用纯文本版本。
            if (child->has_table) {
                budget = child_budget;
                return child;
            }
            if (!candidate.has_body || (!candidate.has_plain && child->has_plain)) {
                candidate = std::move(*child);
            }
            continue;
        }
        budget = child_budget;
        if (!is_mixed) {
            return child;
        }
        for (auto& block : child->blocks) {
            candidate.blocks.push_back(std::move(block));
        }
        candidate.has_body = true;
        candidate.has_plain = candidate.has_plain || child->has_plain;
        candidate.has_table = candidate.has_table || child->has_table;
    }
    if (!candidate.has_body && first_error) {
        return std::unexpected(*first_error);
    }
    return candidate;
}

Result<AttachmentInfo> read_attachment_info(GMimePart* part) {
    AttachmentInfo info;

    // 1. 读取文件名
    if (const char* filename = g_mime_part_get_filename(part)) {
        if (!g_utf8_validate(filename, -1, nullptr)) {
            return parse_failure(error_message::mime::invalid_attachment_filename);
        }
        info.name = filename;
    }

    // 2. 读取 MIME 类型
    GMimeContentType* type = g_mime_object_get_content_type(GMIME_OBJECT(part));
    if (type == nullptr) {
        return parse_failure(error_message::mime::missing_attachment_content_type);
    }

    std::unique_ptr<char, decltype(&g_free)> media_type{
        g_mime_content_type_get_mime_type(type), &g_free
    };
    if (!media_type) {
        return parse_failure(error_message::mime::attachment_media_type_failed);
    }
    info.media_type = media_type.get();

    // 3. 获取附件内容并创建空数据流
    GMimeDataWrapper* content = g_mime_part_get_content(part);
    GMimePtr<GMimeStream> sink{g_mime_stream_null_new()};
    if (content == nullptr || !sink) {
        return parse_failure(error_message::mime::attachment_content_failed);
    }

    // 4. 解码附件并统计字节数
    const auto size = g_mime_data_wrapper_write_to_stream(content, sink.get());
    if (size < 0) {
        return parse_failure(error_message::mime::attachment_decode_failed);
    }
    info.size_bytes = static_cast<std::uint64_t>(size);

    // 5. 返回附件信息
    return info;
}

Status collect_attachments(GMimeObject* part, std::vector<AttachmentInfo>& attachments) {
    // 1. 判断当前部件是否为附件
    if (part == nullptr) {
        return {};
    }
    if (is_attachment(part)) {
        if (!GMIME_IS_PART(part)) {
            return parse_failure(error_message::mime::unsupported_attachment);
        }

        auto info = read_attachment_info(GMIME_PART(part));
        if (!info) {
            return std::unexpected(info.error());
        }

        attachments.push_back(std::move(*info));
        return {};
    }

    // 2. 按原顺序递归遍历子部件
    if (GMIME_IS_MULTIPART(part)) {
        GMimeMultipart* multipart = GMIME_MULTIPART(part);
        const int count = g_mime_multipart_get_count(multipart);

        for (int i = 0; i < count; ++i) {
            GMimeObject* child = g_mime_multipart_get_part(multipart, i);
            auto status = collect_attachments(child, attachments);
            if (!status) {
                return status;
            }
        }
    }

    // 3. 遍历成功
    return {};
}

void read_message_headers(GMimeMessage* message, ParsedMail& result) {
    // 1. 读取 From, 将地址列表格式化为显示文本
    InternetAddressList* from = g_mime_message_get_from(message);
    if (from != nullptr && internet_address_list_length(from) > 0) {
        std::unique_ptr<char, decltype(&g_free)> sender{
            internet_address_list_to_string(from, nullptr, FALSE), &g_free
        };
        if (sender) {
            result.sender = sender.get();
        }
    }

    // 2. 读取 GMime 解码后的主题
    if (const char* subject = g_mime_message_get_subject(message)) {
        result.subject = subject;
    }

    // 3. 读取展开后的会话邮件头，保留尖括号和 ID 顺序
    GMimeObject* message_object = GMIME_OBJECT(message);
    if (const char* message_id = g_mime_object_get_header(message_object, "Message-ID")) {
        result.message_id = message_id;
    }
    if (const char* in_reply_to = g_mime_object_get_header(message_object, "In-Reply-To")) {
        result.in_reply_to = in_reply_to;
    }
    if (const char* references = g_mime_object_get_header(message_object, "References")) {
        result.references = references;
    }
}

}  // namespace

Result<ParsedMail> parse_message(std::string_view raw) {
    auto message = parse_mime_message(raw);
    if (!message) {
        return std::unexpected(message.error());
    }

    // 1. 优先选择纯文本，找不到纯文本时选择 HTML
    GMimeObject* part = g_mime_message_get_mime_part(message->get());
    auto text_parts = select_text_parts(part, "plain");
    const bool is_html = text_parts.empty();

    if (is_html) {
        text_parts = select_text_parts(part, "html");
    }
    if (text_parts.empty()) {
        return parse_failure(error_message::mime::missing_text_body);
    }

    // 2. 逐段解码并拼接正文
    ParsedMail result;
    for (std::size_t i = 0; i < text_parts.size(); ++i) {
        auto body = decode_text_part(text_parts[i]);
        if (!body) {
            return std::unexpected(body.error());
        }

        if (i != 0) {
            result.body.push_back('\n');
        }

        if (is_html) {
            auto text = html_to_text(*body);
            if (!text) {
                return std::unexpected(text.error());
            }
            result.body += *text;
        } else {
            result.body += *body;
        }
    }

    read_message_headers(message->get(), result);

    if (auto status = collect_attachments(part, result.attachments); !status) {
        return std::unexpected(status.error());
    }

    return result;
}

Result<std::vector<BodyBlock>> parse_body_blocks(std::string_view raw) {
    auto message = parse_mime_message(raw);
    if (!message) {
        return std::unexpected(message.error());
    }

    GMimeObject* root = g_mime_message_get_mime_part(message->get());
    if (select_text_parts(root, "html").empty()) {
        return std::vector<BodyBlock>{};
    }

    PresentationBudget budget;
    auto candidate = select_body_candidate(root, budget);
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    if (!candidate->has_table) {
        return std::vector<BodyBlock>{};
    }
    return std::move(candidate->blocks);
}

}  // namespace mailai

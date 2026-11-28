#include "html_extract.h"

#include <glib.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "mailai/error_message.h"

namespace mailai::mime_detail {
namespace {

constexpr std::size_t max_table_columns = 128;

std::unexpected<Error> parse_failure(const char* message) {
    return std::unexpected(Error{
        .code = ErrorCode::parse_error,
        .message = message,
        .account_id = std::nullopt,
        .mail_id = std::nullopt,
        .stage = "mime.parse"
    });
}

// 延迟输出空格和块级换行，避免缩进和嵌套标签引入多余的空白。
enum class HtmlSeparator { none, space, line_break };

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

}  // namespace

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

Result<HtmlBlocks> html_to_blocks(std::string_view html, PresentationBudget& budget) {
    if (html.empty()) {
        return HtmlBlocks{};
    }
    auto document = parse_html_document(html);
    if (!document) {
        return std::unexpected(document.error());
    }

    HtmlPresentation presentation{.budget = budget};
    std::string text;
    HtmlSeparator separator = HtmlSeparator::none;
    append_html_text(xmlDocGetRootElement(document->get()), text, separator, false, &presentation);
    flush_body_text(text, separator, presentation.blocks);
    return HtmlBlocks{
        .blocks = std::move(presentation.blocks), .has_table = presentation.has_table
    };
}

}  // namespace mailai::mime_detail

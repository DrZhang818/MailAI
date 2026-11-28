#include "cli_terminal.h"

#include <locale.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <numeric>
#include <ostream>
#include <utility>

namespace mailai::cli {
namespace {

constexpr std::size_t column_gap = 3;
constexpr std::string_view ellipsis = "…";
constexpr std::string_view line = "─";
constexpr std::string_view header_color = "\x1b[1;33m";
constexpr std::string_view separator_color = "\x1b[2m";
constexpr std::string_view reset_color = "\x1b[0m";

struct CodePoint {
    std::uint32_t value;
    std::size_t length;
};

CodePoint read_code_point(std::string_view text) {
    const auto first = static_cast<unsigned char>(text.front());
    if (first < 0x80)
        return {first, 1};
    const std::size_t count = first >= 0xC2 && first <= 0xDF   ? 2
                              : first >= 0xE0 && first <= 0xEF ? 3
                              : first >= 0xF0 && first <= 0xF4 ? 4
                                                               : 0;
    if (count == 0 || count > text.size())
        return {first, 0};

    std::uint32_t value = first & (0x7F >> count);
    for (std::size_t i = 1; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[i]);
        if ((next & 0xC0) != 0x80)
            return {first, 0};
        value = (value << 6) | (next & 0x3F);
    }
    const auto minimum = count == 2 ? 0x80U : count == 3 ? 0x800U : 0x10000U;
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        return {first, 0};
    return {value, count};
}

bool terminal_control(std::uint32_t value) {
    return value < 0x20 || (value >= 0x7F && value <= 0x9F) || value == 0x061C || value == 0x200E ||
           value == 0x200F || (value >= 0x2028 && value <= 0x202E) ||
           (value >= 0x2066 && value <= 0x2069);
}

std::string escaped_code_point(std::uint32_t value) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string escaped = value <= 0xFF ? "\\x" : "\\u";
    if (value > 0xFF) {
        escaped += digits[(value >> 12) & 0xF];
        escaped += digits[(value >> 8) & 0xF];
    }
    escaped += digits[(value >> 4) & 0xF];
    escaped += digits[value & 0xF];
    return escaped;
}

// 使用独立的线程区域设置测量 UTF-8 显示宽度，不改变进程的全局区域设置。
class Utf8Locale {
public:
    Utf8Locale() {
        locale_ = ::newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
        if (!locale_)
            locale_ = ::newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr);
        if (locale_)
            previous_ = ::uselocale(locale_);
    }
    ~Utf8Locale() {
        if (previous_)
            ::uselocale(previous_);
        if (locale_)
            ::freelocale(locale_);
    }
    Utf8Locale(const Utf8Locale&) = delete;
    Utf8Locale& operator=(const Utf8Locale&) = delete;

private:
    locale_t locale_{};
    locale_t previous_{};
};

std::size_t code_point_width(std::uint32_t value) {
    const int width = ::wcwidth(static_cast<wchar_t>(value));
    return width < 0 ? 1 : static_cast<std::size_t>(width);
}

struct Cell {
    std::string text;
    std::size_t width{};
};

struct Glyph {
    std::string text;
    std::size_t width;
    std::size_t length;
};

Glyph next_glyph(std::string_view text) {
    const auto point = read_code_point(text);
    const auto length = point.length == 0 ? 1 : point.length;
    if (point.length == 0 || terminal_control(point.value)) {
        auto escaped = escaped_code_point(point.value);
        const auto width = escaped.size();
        return {std::move(escaped), width, length};
    }
    return {std::string{text.substr(0, length)}, code_point_width(point.value), length};
}

// 截断以完整字符和转义单元为边界，避免破坏 UTF-8 或终端显示。
Cell fit_cell(std::string_view text, std::size_t limit) {
    Cell cell;
    std::size_t previous_offset{};
    std::size_t previous_width{};
    while (!text.empty()) {
        const auto [glyph, width, length] = next_glyph(text);

        if (width > limit - cell.width) {
            if (limit != 0) {
                if (cell.width == limit) {
                    cell.text.resize(previous_offset);
                    cell.width = previous_width;
                }
                cell.text += ellipsis;
                ++cell.width;
            }
            break;
        }
        if (width != 0) {
            previous_offset = cell.text.size();
            previous_width = cell.width;
        }
        cell.text += glyph;
        cell.width += width;
        text.remove_prefix(length);
    }
    return cell;
}

// 自动换行保留所有字符；仅在极窄的列中将纯 ASCII 转义序列分到多行。
std::vector<Cell> wrap_cell(std::string_view text, std::size_t limit, bool multiline) {
    const auto width_limit = std::max<std::size_t>(limit, 2);
    std::vector<Cell> lines(1);
    while (!text.empty()) {
        if (multiline && (text.starts_with("\r\n") || text.front() == '\n')) {
            text.remove_prefix(text.front() == '\r' ? 2 : 1);
            lines.emplace_back();
            continue;
        }
        auto [glyph, width, length] = next_glyph(text);
        if (multiline && text.front() == '\t') {
            glyph = " ";
            width = 1;
        }
        if (width > width_limit) {
            for (char ch : glyph) {
                if (lines.back().width == width_limit)
                    lines.emplace_back();
                lines.back().text += ch;
                ++lines.back().width;
            }
        } else {
            if (width > width_limit - lines.back().width)
                lines.emplace_back();
            lines.back().text += glyph;
            lines.back().width += width;
        }
        text.remove_prefix(length);
    }
    return lines;
}

std::size_t column_limit(const TableColumn& column) {
    return column.max_width == 0 ? std::numeric_limits<std::size_t>::max() : column.max_width;
}

std::vector<std::size_t> table_widths(
    std::span<const TableColumn> columns, std::span<const TableRow> rows, std::size_t available) {
    std::vector<std::size_t> minimum;
    std::vector<std::size_t> widths;
    minimum.reserve(columns.size());
    widths.reserve(columns.size());
    for (const auto& column : columns) {
        auto limit = std::min(column_limit(column), std::max<std::size_t>(available, 2));
        auto width = fit_cell(column.title, limit).width;
        minimum.push_back(
            column.wrap ? std::min<std::size_t>(std::max(width, std::size_t{2}), 6) : width);
        widths.push_back(width);
    }
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const auto limit = columns[i].shrinkable ? std::min(column_limit(columns[i]), available)
                                                     : column_limit(columns[i]);
            widths[i] = std::max(widths[i], fit_cell(row.at(i), limit).width);
        }
    }
    for (std::size_t i = 0; i < widths.size(); ++i) widths[i] = std::max(widths[i], minimum[i]);
    auto total = std::accumulate(widths.begin(), widths.end(), column_gap * (columns.size() - 1));
    // 优先压缩可缩短的列，完整保留本地 ID 和历史、解析状态。
    while (total > available) {
        std::size_t widest = columns.size();
        std::size_t surplus = 0;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].shrinkable && widths[i] - minimum[i] > surplus) {
                widest = i;
                surplus = widths[i] - minimum[i];
            }
        }
        if (widest == columns.size())
            break;
        --widths[widest];
        --total;
    }
    return widths;
}

void print_cell(std::ostream& out, const Cell& cell, std::size_t width, bool last) {
    out << cell.text;
    if (!last)
        out << std::string(width - cell.width + column_gap, ' ');
}

void print_row(std::ostream& out,
    std::span<const TableColumn> columns,
    const TableRow& row,
    const std::vector<std::size_t>& widths) {
    std::vector<std::vector<Cell>> cells;
    std::size_t height = 1;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        cells.push_back(columns[i].wrap ? wrap_cell(row.at(i), widths[i], columns[i].multiline)
                                        : std::vector<Cell>{fit_cell(row.at(i), widths[i])});
        height = std::max(height, cells.back().size());
    }
    for (std::size_t line_index = 0; line_index < height; ++line_index) {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const Cell empty;
            print_cell(out,
                line_index < cells[i].size() ? cells[i][line_index] : empty,
                widths[i],
                i + 1 == columns.size());
        }
        out << '\n';
    }
}

void print_divider(std::ostream& out, const std::vector<std::size_t>& widths, bool color) {
    if (color)
        out << separator_color;
    for (std::size_t i = 0; i < widths.size(); ++i) {
        if (i != 0)
            out << std::string(column_gap, ' ');
        for (std::size_t count = 0; count < widths[i]; ++count) out << line;
    }
    if (color)
        out << reset_color;
    out << '\n';
}

// 极窄的终端无法容纳所有列时，按记录纵向展示，不截断 ID 和状态。
void print_stacked(std::ostream& out,
    std::span<const TableColumn> columns,
    std::span<const TableRow> rows,
    const TerminalOptions& terminal) {
    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
        if (row_index != 0)
            print_divider(out, {terminal.columns}, terminal.color);
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].title.empty()) {
                for (const auto& cell :
                    wrap_cell(rows[row_index].at(i), terminal.columns, columns[i].multiline))
                    out << cell.text << '\n';
                continue;
            }
            const auto heading = fit_cell(columns[i].title, column_limit(columns[i]));
            if (columns[i].wrap && heading.width + 2 >= terminal.columns) {
                if (terminal.color)
                    out << header_color;
                for (const auto& line :
                    wrap_cell(columns[i].title, terminal.columns, columns[i].multiline))
                    out << line.text << '\n';
                if (terminal.color)
                    out << reset_color;
                for (const auto& line :
                    wrap_cell(rows[row_index].at(i), terminal.columns, columns[i].multiline))
                    out << line.text << '\n';
                continue;
            }
            if (terminal.color)
                out << header_color;
            out << heading.text;
            if (terminal.color)
                out << reset_color;
            const auto offset = heading.width + 2;
            auto limit = column_limit(columns[i]);
            bool separate_line =
                terminal.columns <= offset || (columns[i].wrap && terminal.columns - offset < 2);
            if (columns[i].shrinkable) {
                limit =
                    std::min(limit, separate_line ? terminal.columns : terminal.columns - offset);
            }
            if (columns[i].wrap) {
                const auto lines = wrap_cell(rows[row_index].at(i), limit, columns[i].multiline);
                out << (separate_line ? ":\n" : ": ") << lines.front().text << '\n';
                for (std::size_t j = 1; j < lines.size(); ++j) {
                    if (!separate_line)
                        out << std::string(offset, ' ');
                    out << lines[j].text << '\n';
                }
                continue;
            }
            const auto cell = fit_cell(rows[row_index].at(i), limit);
            separate_line = separate_line || cell.width + offset > terminal.columns;
            out << (separate_line ? ":\n" : ": ") << cell.text << '\n';
        }
    }
}

}  // namespace

TerminalOptions detect_terminal_options() {
    TerminalOptions terminal;
    if (!::isatty(STDOUT_FILENO))
        return terminal;
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col != 0)
        terminal.columns = size.ws_col;
    const char* term = std::getenv("TERM");
    terminal.color = term && std::string_view{term} != "dumb" && std::getenv("NO_COLOR") == nullptr;
    return terminal;
}

void print_text(std::ostream& out, std::string_view text, bool multiline) {
    while (!text.empty()) {
        if (multiline && text.starts_with("\r\n")) {
            out << '\n';
            text.remove_prefix(2);
            continue;
        }
        const auto point = read_code_point(text);
        if (point.length == 0) {
            out << escaped_code_point(point.value);
            text.remove_prefix(1);
            continue;
        }
        if (terminal_control(point.value) && !(multiline && point.value == '\n'))
            out << escaped_code_point(point.value);
        else
            out << text.substr(0, point.length);
        text.remove_prefix(point.length);
    }
}

void print_table(std::ostream& out,
    std::span<const TableColumn> columns,
    std::span<const TableRow> rows,
    const TerminalOptions& terminal) {
    if (columns.empty())
        return;
    const Utf8Locale locale;
    const auto widths = table_widths(columns, rows, terminal.columns);
    if (std::accumulate(widths.begin(), widths.end(), column_gap * (columns.size() - 1)) >
        terminal.columns) {
        print_stacked(out, columns, rows, terminal);
        return;
    }
    TableRow headings;
    for (const auto& column : columns) headings.emplace_back(column.title);
    const bool has_header =
        std::ranges::any_of(headings, [](const auto& title) { return !title.empty(); });
    if (has_header) {
        if (terminal.color)
            out << header_color;
        print_row(out, columns, headings, widths);
        if (terminal.color)
            out << reset_color;
    }
    bool divider = has_header;
    for (const auto& row : rows) {
        if (divider)
            print_divider(out, widths, terminal.color);
        print_row(out, columns, row, widths);
        divider = true;
    }
}

}  // namespace mailai::cli

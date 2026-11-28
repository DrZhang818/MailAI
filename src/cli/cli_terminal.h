#pragma once

#include <cstddef>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mailai::cli {

struct TerminalOptions {
    std::size_t columns{120};
    bool color{false};
};

struct TableColumn {
    std::string_view title;
    std::size_t max_width{};
    bool shrinkable{true};
    bool wrap{false};
    bool multiline{false};
};

using TableRow = std::vector<std::string>;

TerminalOptions detect_terminal_options();

void print_text(std::ostream& out, std::string_view text, bool multiline = false);

void print_table(std::ostream& out,
    std::span<const TableColumn> columns,
    std::span<const TableRow> rows,
    const TerminalOptions& terminal);

}  // namespace mailai::cli

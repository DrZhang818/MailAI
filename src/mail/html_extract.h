#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "mailai/model.h"

namespace mailai::mime_detail {

// HTML 表格展开的总预算：单元格总数，以及跨行单元格重复文字的总字节数。
inline constexpr std::size_t max_table_cells = 100000;
inline constexpr std::size_t max_repeated_table_text_bytes = 20 * 1024 * 1024;

struct PresentationBudget {
    std::size_t cells{max_table_cells};
    std::size_t repeated_bytes{max_repeated_table_text_bytes};
};

struct HtmlBlocks {
    std::vector<BodyBlock> blocks;
    bool has_table{};
};

// 将 HTML 正文转换为纯文本，块级元素换行，表格单元格以制表符分隔。
Result<std::string> html_to_text(std::string_view html);

// 将 HTML 正文转换为文本块和结构化表格；成功提取的表格会消耗预算。
Result<HtmlBlocks> html_to_blocks(std::string_view html, PresentationBudget& budget);

}  // namespace mailai::mime_detail

#include "mailai/mime.h"

#include <cstddef>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "test_support.h"

namespace {

mailai::ParsedMail parse_raw(std::string_view raw) {
    auto result = mailai::parse_message(raw);
    if (!result) {
        throw std::runtime_error("expected successful MIME parsing: " + result.error().message);
    }
    return std::move(*result);
}

mailai::ParsedMail parse_sample(const char* filename) {
    std::ifstream input(filename, std::ios::binary);
    expect(input.is_open(), "could not open sample");
    const std::string raw{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    return parse_raw(raw);
}

void expect_plain_message(const char* filename, std::string_view expected_body) {
    const auto mail = parse_sample(filename);
    expect(mail.subject == "会议提醒", "subject should be decoded as UTF-8");
    expect(mail.body == expected_body, "plain-text body should match the expected UTF-8 bytes");
}

void expect_reply_headers(const char* filename) {
    const auto mail = parse_sample(filename);
    expect(mail.sender == "发件人 <sender@example.test>",
        "From should include the decoded display name and address");
    expect(mail.subject == "会议提醒", "subject should be decoded as UTF-8");
    expect(mail.message_id == "<reply@example.test>", "Message-ID should retain its brackets");
    expect(mail.in_reply_to == "<parent@example.test>", "In-Reply-To should retain its brackets");
    expect(mail.references == "<root@example.test> <parent@example.test>",
        "References should be unfolded and preserve the order of the IDs");
}

void expect_missing_headers(const char* filename) {
    const auto mail = parse_sample(filename);
    expect(mail.sender.empty(), "missing From should leave the sender empty");
    expect(mail.subject.empty(), "missing Subject should leave the subject empty");
    expect(mail.message_id.empty() && mail.in_reply_to.empty() && mail.references.empty(),
        "missing thread headers should leave the IDs empty");
    expect(
        mail.body == "这封邮件只有正文。\n", "missing headers should still allow reading the body");
}

void expect_html_text(std::string_view html, std::string_view expected) {
    std::string raw = "Content-Type: text/html; charset=UTF-8\n\n";
    raw += html;
    const auto mail = mailai::parse_message(raw);
    expect(mail.has_value(), "HTML mail should parse successfully");
    expect(mail->body == expected, "HTML text should match the expected UTF-8 bytes");
}

std::vector<mailai::BodyBlock> parse_blocks(std::string_view raw) {
    auto result = mailai::parse_body_blocks(raw);
    if (!result) {
        throw std::runtime_error(
            "expected successful structured MIME parsing: " + result.error().message);
    }
    return std::move(*result);
}

void expect_text_block(const mailai::BodyBlock& block, std::string_view expected) {
    const auto* text = std::get_if<std::string>(&block);
    expect(text != nullptr && *text == expected, "unexpected body text block");
}

void expect_table_block(
    const mailai::BodyBlock& block, const std::vector<std::vector<std::string>>& expected) {
    const auto* table = std::get_if<mailai::BodyTable>(&block);
    expect(table != nullptr && table->rows == expected, "unexpected body table rows");
}

void expect_structured_table_alternative() {
    const std::string raw =
        "MIME-Version: 1.0\nContent-Type: multipart/alternative; boundary=alt\n\n"
        "--alt\nContent-Type: text/plain; charset=UTF-8\n\n纯文本备用正文\n"
        "--alt\nContent-Type: multipart/related; boundary=related\n\n"
        "--related\nContent-Type: text/html; charset=UTF-8\n\n"
        "<body><p>导语</p><table><tr><th>分类</th><th colspan='2'>具体内容</th></tr>"
        "<tr><td rowspan='2'><p>项目</p></td><td><div>甲</div></td><td>乙</td></tr>"
        "<tr><td><p>丙</p><div>丁</div></td><td>戊</td></tr></table>"
        "<p>结语<img src='cid:chart'><img src='https://example.test/chart.png'></p></body>\n"
        "--related\nContent-Type: image/png\nContent-ID: <chart>\n"
        "Content-Transfer-Encoding: base64\n\naW1hZ2U=\n--related--\n--alt--\n";

    expect(parse_raw(raw).body == "纯文本备用正文", "stored body should still prefer plain text");

    const auto blocks = parse_blocks(raw);
    expect(blocks.size() == 3, "structured HTML alternative should preserve surrounding text");
    expect_text_block(blocks[0], "导语");
    expect_table_block(
        blocks[1], {{"分类", "具体内容", ""}, {"项目", "甲", "乙"}, {"项目", "丙\n丁", "戊"}});
    expect_text_block(blocks[2], "结语");
}

void expect_nested_table_and_mixed_text() {
    const auto blocks = parse_blocks(
        "Content-Type: multipart/mixed; boundary=mixed\n\n"
        "--mixed\nContent-Type: text/plain; charset=UTF-8\n\n前置内容\n"
        "--mixed\nContent-Type: text/html; charset=UTF-8\n\n"
        "<table><tr><td><p>说明</p><table><caption>统计表</caption>"
        "<thead><tr><th>名称</th><th>数量</th></tr></thead>"
        "<tbody><tr><td>甲</td><td>2</td></tr><tr><td>乙</td><td>3</td></tr></tbody>"
        "</table><p>注释</p></td></tr></table>\n"
        "--mixed\nContent-Type: text/plain; charset=UTF-8\n\n后续内容\n--mixed--\n");
    expect(blocks.size() == 5, "nested data table should retain mixed siblings and layout text");
    expect_text_block(blocks[0], "前置内容");
    expect_text_block(blocks[1], "说明\n统计表");
    expect_table_block(blocks[2], {{"名称", "数量"}, {"甲", "2"}, {"乙", "3"}});
    expect_text_block(blocks[3], "注释");
    expect_text_block(blocks[4], "后续内容");
}

void expect_bounded_table_spans() {
    const auto blocks = parse_blocks(
        "Content-Type: text/html; charset=UTF-8\n\n"
        "<table><thead><tr><th rowspan='0'>标题</th><th>值</th></tr>"
        "<tr><th>说明</th></tr></thead><tbody>"
        "<tr><td colspan='99999999999999999999'>甲</td><td "
        "rowspan='99999999999999999999'>1</td></tr>"
        "<tr><td>乙</td><td>2</td></tr></tbody></table>");
    expect(blocks.size() == 1, "excessive span attributes should not invalidate a readable table");
    expect_table_block(blocks[0], {{"标题", "值"}, {"标题", "说明"}, {"甲", "1"}, {"乙", "2"}});
}

void expect_shared_table_spans() {
    const auto blocks = parse_blocks(
        "Content-Type: text/html; charset=UTF-8\n\n"
        "<table><tr><td>甲</td><td rowspan='2' colspan='2'>合计</td><td>乙</td></tr>"
        "<tr><td>丙</td><td>丁</td></tr></table>");
    expect(blocks.size() == 1, "combined spans should produce one table");
    expect_table_block(blocks[0], {{"甲", "合计", "", "乙"}, {"丙", "合计", "", "丁"}});
}

void expect_overwide_table_text_fallback() {
    std::string raw =
        "Content-Type: text/html; charset=UTF-8\n\n"
        "<table><tr><td>名称</td><td>数量</td></tr><tr><td>甲</td><td>1</td></tr></table>"
        "<table><tr>";
    for (int i = 0; i < 140; ++i) {
        raw += "<td>列" + std::to_string(i) + "</td>";
    }
    raw += "</tr></table><p>末尾正文</p>";

    const auto blocks = parse_blocks(raw);
    expect(blocks.size() == 2, "overwide table should not expand the structured grid");
    expect_table_block(blocks[0], {{"名称", "数量"}, {"甲", "1"}});
    const auto* remainder = std::get_if<std::string>(&blocks[1]);
    expect(remainder != nullptr && remainder->find("列0") != std::string::npos &&
               remainder->find("列139") != std::string::npos &&
               remainder->find("末尾正文") != std::string::npos,
        "overwide table fallback should keep all cell text and following content");
}

void expect_cumulative_table_cell_limit() {
    std::string raw = "Content-Type: text/html; charset=UTF-8\n\n";
    // 两个 500 × 100 的表格刚好用完 100,000 个单元格的预算。
    for (int table = 0; table < 2; ++table) {
        raw += "<table>";
        for (int row = 0; row < 500; ++row) {
            raw += "<tr><td colspan='100'>cell</td></tr>";
        }
        raw += "</table>";
    }
    raw += "<table><tr><td>overflow cell</td></tr></table>";

    const auto blocks = parse_blocks(raw);
    expect(blocks.size() == 3, "the cell after the cumulative limit should become text");
    for (std::size_t index = 0; index < 2; ++index) {
        const auto* table = std::get_if<mailai::BodyTable>(&blocks[index]);
        expect(table && table->rows.size() == 500 && table->rows.back().size() == 100,
            "a complete table at the cumulative cell limit should remain structured");
    }
    expect_text_block(blocks.back(), "overflow cell");
}

void expect_rowspan_repeated_byte_limit() {
    const std::string repeated(20 * 1024, 'a');
    std::string raw =
        "Content-Type: text/html; charset=UTF-8\n\n"
        "<table><tr><td rowspan='0'>";
    raw += repeated + "</td></tr>";
    // 跨行复制 1,024 次，恰好消耗 20 MiB；下一张表再复制 1 字节时应回退成文本。
    for (int row = 0; row < 1024; ++row) {
        raw += "<tr><td>row</td></tr>";
    }
    raw +=
        "</table><table><tr><td rowspan='2'>x</td></tr>"
        "<tr><td>overflow rowspan</td></tr></table>";

    const auto blocks = parse_blocks(raw);
    expect(blocks.size() == 2, "only the table beyond the repeated-byte limit should become text");
    const auto* table = std::get_if<mailai::BodyTable>(&blocks.front());
    expect(table && table->rows.size() == 1025 && !table->rows.back().empty() &&
               table->rows.back().front() == repeated,
        "a rowspan at the repeated-byte limit should retain its last copy");
    expect_text_block(blocks.back(), "x\noverflow rowspan");
}

void expect_body_blocks_fallback() {
    expect(parse_blocks("Content-Type: text/plain; charset=UTF-8\n\n正文").empty(),
        "plain text should use the existing stored body");
    expect(parse_blocks("Content-Type: text/html; charset=UTF-8\n\n<p>正文</p>").empty(),
        "HTML without data tables should use the existing stored body");
    expect(parse_blocks("Content-Type: text/html; charset=UTF-8\n\n"
                        "<table><tr><td><img src='cid:chart'></td></tr></table>")
               .empty(),
        "image-only tables should use the existing stored body");
    expect(parse_blocks("Content-Type: multipart/mixed; boundary=test\n\n"
                        "--test\nContent-Type: text/plain\n\nplain\n"
                        "--test\nContent-Type: text/html\n"
                        "Content-Disposition: attachment; filename=table.html\n\n"
                        "<table><tr><td>attachment</td></tr></table>\n--test--\n")
               .empty(),
        "HTML attachments should not replace the existing stored body");
}

void expect_parse_error(std::string_view raw, std::string_view reason) {
    const auto result = mailai::parse_message(raw);
    expect(!result, "invalid MIME message should fail");
    expect(result.error().code == mailai::ErrorCode::parse_error, "expected parse_error");
    expect(result.error().stage == "mime.parse", "expected MIME parsing error stage");
    if (result.error().message.find(reason) == std::string::npos) {
        throw std::runtime_error("unexpected parse error: " + result.error().message);
    }
}

void expect_nested_multipart() {
    const auto mail = parse_raw(
        "MIME-Version: 1.0\n"
        "Content-Type: multipart/mixed; boundary=outer\n\n"
        "--outer\n"
        "Content-Type: multipart/alternative; boundary=inner\n\n"
        "--inner\n"
        "Content-Type: text/plain; charset=UTF-8\n\n旧版本\n"
        "--inner\n"
        "Content-Type: text/plain; charset=UTF-8\n\n第一段\n"
        "--inner\n"
        "Content-Type: text/html; charset=UTF-8\n\n<p>HTML版本</p>\n"
        "--inner--\n"
        "--outer\n"
        "Content-Type: application/octet-stream\n"
        "Content-Disposition: attachment; filename*=UTF-8''%E9%99%84%E4%BB%B6.bin\n"
        "Content-Transfer-Encoding: base64\n\nAAEC/w==\n"
        "--outer\n"
        "Content-Type: multipart/mixed; boundary=second\n\n"
        "--second\n"
        "Content-Type: text/plain; charset=UTF-8\n\n第二段\n"
        "--second\n"
        "Content-Type: text/plain; charset=UTF-8\n"
        "Content-Disposition: attachment; filename=note.txt\n"
        "Content-Transfer-Encoding: quoted-printable\n\n=E4=B8=AD\n"
        "--second--\n"
        "--outer\n"
        "Content-Type: application/octet-stream; name=legacy.bin\n"
        "Content-Transfer-Encoding: base64\n\nYQ==\n"
        "--outer\n"
        "Content-Type: image/png\n"
        "Content-Disposition: inline; filename=inline.png\n"
        "Content-Transfer-Encoding: base64\n\nYQ==\n"
        "--outer\n"
        "Content-Type: application/octet-stream\n"
        "Content-Disposition: attachment\n"
        "Content-Transfer-Encoding: base64\n\nYWI=\n"
        "--outer--\n");

    expect(mail.body == "第一段\n第二段", "nested plain-text parts should be selected in order");
    expect(mail.attachments.size() == 4, "should collect attachments, not inline resources");
    const auto& attachments = mail.attachments;
    expect(attachments[0].name == "附件.bin" &&
               attachments[0].media_type == "application/octet-stream" &&
               attachments[0].size_bytes == 4,
        "should decode the attachment filename and count decoded binary bytes");
    expect(attachments[1].name == "note.txt" && attachments[1].media_type == "text/plain" &&
               attachments[1].size_bytes == 3,
        "should collect nested text attachments and count quoted-printable decoded bytes");
    expect(attachments[2].name == "legacy.bin" &&
               attachments[2].media_type == "application/octet-stream" &&
               attachments[2].size_bytes == 1,
        "should recognize an attachment named by Content-Type");
    expect(attachments[3].name.empty() && attachments[3].media_type == "application/octet-stream" &&
               attachments[3].size_bytes == 2,
        "should allow an unnamed attachment");
}

void expect_raw_size_limit() {
    constexpr std::size_t max_size = 20 * 1024 * 1024;
    const std::string header = "Content-Type: text/plain; charset=UTF-8\n\n";
    std::string raw = header;
    raw.resize(max_size, 'a');
    const auto mail = parse_raw(raw);
    expect(mail.body.size() == max_size - header.size(), "should parse exactly 20 MiB");
    raw.push_back('a');
    expect_parse_error(raw, "size limit");
}

std::string nested_message(std::size_t depth) {
    std::string raw = "MIME-Version: 1.0\n";
    for (std::size_t i = 1; i < depth; ++i) {
        const auto boundary = "nest-" + std::to_string(i) + "-end";
        raw += "Content-Type: multipart/mixed; boundary=" + boundary + "\n\n--" + boundary + "\n";
    }
    raw += "Content-Type: text/plain; charset=UTF-8\n\n正文\n";
    for (std::size_t i = depth - 1; i > 0; --i) {
        raw += "--nest-" + std::to_string(i) + "-end--\n";
    }
    return raw;
}

std::string multipart_message(std::size_t total_parts) {
    // 根 multipart 和所有未选中的替代正文部件均计入部件总数。
    std::string raw = "Content-Type: multipart/alternative; boundary=parts\n\n";
    for (std::size_t i = 1; i < total_parts; ++i) {
        raw += "--parts\nContent-Type: text/plain; charset=UTF-8\n\n正文\n";
    }
    raw += "--parts--\n";
    return raw;
}

}  // namespace

int main() {
    const char* current_sample = "plain_utf8.eml";
    try {
        expect_plain_message(current_sample, "请在下午三点参加会议。\n");

        current_sample = "plain_gb18030_base64.eml";
        expect_plain_message(current_sample, "请在下午三点参加会议。");

        current_sample = "multipart_alternative.eml";
        expect_plain_message(current_sample, "请在下午三点参加会议。");

        current_sample = "multipart_alternative_two_plain.eml";
        expect_plain_message(current_sample, "请在下午三点参加会议。");

        current_sample = "multipart_mixed_text_attachment.eml";
        expect_plain_message(current_sample, "请在下午三点参加会议。\n会议地点是 301 教室。");

        current_sample = "headers_missing.eml";
        expect_missing_headers(current_sample);

        current_sample = "headers_reply.eml";
        expect_reply_headers(current_sample);

        current_sample = "HTML whitespace";
        expect_html_text(
            "<body>\n  <p>  Hello <span>world</span> \n <b>again</b>!</p>\n"
            "<p>second<br> \n third<br><br> fourth</p>\n</body>",
            "Hello world again!\nsecond\nthird\n\nfourth\n");

        current_sample = "HTML blocks and pre";
        expect_html_text(
            "<h1>Title</h1><h6>Sub</h6><blockquote>quote</blockquote>"
            "<pre>  x \n    <b>y </b></pre><p> end </p>",
            "Title\nSub\nquote\n  x \n    y \nend\n");

        current_sample = "HTML table cells";
        expect_html_text(
            "<table><tr><th> A </th>\n <th>B</th></tr>"
            "<tr><td><p>C</p></td><td><p>D</p></td></tr>"
            "<tr><td>E</td><td></td></tr><tr><td></td><td>F</td></tr></table>",
            "A\tB\nC\tD\nE\t\n\tF\n");

        current_sample = "structured HTML alternative with row and column spans";
        expect_structured_table_alternative();

        current_sample = "nested table with mixed text";
        expect_nested_table_and_mixed_text();

        current_sample = "bounded table spans";
        expect_bounded_table_spans();

        current_sample = "combined row and column spans";
        expect_shared_table_spans();

        current_sample = "overwide table fallback";
        expect_overwide_table_text_fallback();

        current_sample = "cumulative HTML table cell limit";
        expect_cumulative_table_cell_limit();

        current_sample = "rowspan repeated-byte limit";
        expect_rowspan_repeated_byte_limit();

        current_sample = "structured body fallback";
        expect_body_blocks_fallback();

        current_sample = "HTML hidden subtrees and useful fallback text";
        expect_html_text(
            "<html><head><title>ignore</title></head><body><p>正文</p>"
            "<template><p>ignore</p></template><script>ignore</script><style>ignore</style>"
            "<noscript>备用</noscript><svg><text>图</text></svg></body></html>",
            "正文\n备用图");

        current_sample = "HTML significant whitespace";
        expect_html_text("<p>甲<span> </span><b>乙</b>&nbsp;丙</p>", "甲 乙\u00a0丙\n");

        current_sample = "nested multipart and attachment metadata";
        expect_nested_multipart();

        current_sample = "quoted-printable UTF-8 body";
        expect(parse_raw("Content-Type: text/plain; charset=UTF-8\n"
                         "Content-Transfer-Encoding: quoted-printable\n\n=E4=BD=A0=E5=A5=BD")
                       .body == "你好",
            "should decode a quoted-printable Chinese body");

        current_sample = "unsupported plain-text and HTML charsets";
        expect_parse_error(
            "Content-Type: text/plain; charset=x-mailai-unknown\n\nHello", "charset");
        expect_parse_error(
            "Content-Type: text/html; charset=x-mailai-unknown\n\n<p>Hello</p>", "charset");
        expect_plain_message("plain_utf8.eml", "请在下午三点参加会议。\n");

        current_sample = "raw size boundary";
        expect_raw_size_limit();

        current_sample = "MIME depth boundary";
        expect(parse_raw(nested_message(32)).body == "正文", "should allow 32 MIME levels");
        expect_parse_error(nested_message(33), "depth limit");

        current_sample = "MIME part count boundary";
        expect(parse_raw(multipart_message(256)).body == "正文", "should allow 256 MIME parts");
        expect_parse_error(multipart_message(257), "part count limit");
    } catch (const std::exception& error) {
        std::cerr << current_sample << ": " << error.what() << '\n';
        return 1;
    }
}

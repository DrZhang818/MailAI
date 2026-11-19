#include "mailai/mime.h"

#include <gmime/gmime.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace mailai {
namespace {

constexpr std::size_t max_raw_bytes = 20 * 1024 * 1024;
constexpr std::size_t max_mime_depth = 32;
constexpr std::size_t max_mime_parts = 256;

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
        return parse_failure("Missing MIME part");
    }
    if (depth > max_mime_depth) {
        return parse_failure("MIME depth limit exceeded");
    }

    // 2. 检查部件总数
    if (++count > max_mime_parts) {
        return parse_failure("MIME part count limit exceeded");
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
            return parse_failure("Missing embedded message");
        }
        return validate_mime_structure(g_mime_message_get_mime_part(embedded), depth + 1, count);
    }

    return {};
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
            return parse_failure("Unknown or unsupported text charset");
        }

        // 2. 检查是否支持转化为 UTF-8
        GMimePtr<GMimeFilter> filter{g_mime_filter_charset_new(charset, "UTF-8")};
        if (!filter) {
            return parse_failure("Unknown or unsupported text charset");
        }
    }

    // 3. 读取解码后的文本并校验 UTF-8
    std::unique_ptr<char, decltype(&g_free)> text{g_mime_text_part_get_text(part), &g_free};
    if (!text || !g_utf8_validate(text.get(), -1, nullptr)) {
        return parse_failure("Cannot read a UTF-8 body");
    }

    // 4. 复制文本并返回
    return std::string{text.get()};
}

// 延迟输出空格和块级换行，避免缩进和嵌套标签引入多余的空白。
enum class HtmlSeparator { none, space, line_break };

bool is_html_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
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
    bool preserve_whitespace = false) {
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

        const std::string_view name{reinterpret_cast<const char*>(node->name)};
        if (name == "head" || name == "script" || name == "style" || name == "template") {
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

        append_html_text(node->children, text, separator, preserve_whitespace || name == "pre");

        if (is_block) {
            separator = HtmlSeparator::line_break;
        }
    }
}

Result<std::string> html_to_text(std::string_view html) {
    // 1. 检查输入
    if (html.empty()) {
        return std::string{};
    }
    if (html.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return parse_failure("HTML is too large");
    }

    // 2. 解析 HTML 并取得根节点
    const int options = HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING;
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document{
        htmlReadMemory(html.data(), static_cast<int>(html.size()), nullptr, "UTF-8", options),
        &xmlFreeDoc
    };
    if (!document) {
        return parse_failure("Cannot parse HTML");
    }

    const xmlNode* root = xmlDocGetRootElement(document.get());
    if (root == nullptr) {
        return parse_failure("HTML has no root element");
    }

    // 3. 遍历文档树，提取并格式化文本
    std::string text;
    HtmlSeparator separator = HtmlSeparator::none;
    append_html_text(root, text, separator);
    if (separator == HtmlSeparator::line_break) {
        flush_html_separator(text, separator);
    }

    // 4. 返回提取结果
    return text;
}

Result<AttachmentInfo> read_attachment_info(GMimePart* part) {
    AttachmentInfo info;

    // 1. 读取文件名
    if (const char* filename = g_mime_part_get_filename(part)) {
        if (!g_utf8_validate(filename, -1, nullptr)) {
            return parse_failure("Attachment filename is not UTF-8");
        }
        info.name = filename;
    }

    // 2. 读取 MIME 类型
    GMimeContentType* type = g_mime_object_get_content_type(GMIME_OBJECT(part));
    if (type == nullptr) {
        return parse_failure("Missing attachment content type");
    }

    std::unique_ptr<char, decltype(&g_free)> media_type{
        g_mime_content_type_get_mime_type(type), &g_free
    };
    if (!media_type) {
        return parse_failure("Cannot read attachment media type");
    }
    info.media_type = media_type.get();

    // 3. 获取附件内容并创建空数据流
    GMimeDataWrapper* content = g_mime_part_get_content(part);
    GMimePtr<GMimeStream> sink{g_mime_stream_null_new()};
    if (content == nullptr || !sink) {
        return parse_failure("Cannot read attachment content");
    }

    // 4. 解码附件并统计字节数
    const auto size = g_mime_data_wrapper_write_to_stream(content, sink.get());
    if (size < 0) {
        return parse_failure("Cannot decode attachment content");
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
            return parse_failure("Unsupported attachment type");
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
    if (raw.empty()) {
        return parse_failure("Empty message");
    }
    if (raw.size() > max_raw_bytes) {
        return parse_failure("Message size limit exceeded");
    }

    static std::once_flag gmime_init_flag;
    std::call_once(gmime_init_flag, g_mime_init);

    GMimePtr<GMimeStream> stream{g_mime_stream_mem_new_with_buffer(raw.data(), raw.size())};
    GMimePtr<GMimeParser> parser{g_mime_parser_new_with_stream(stream.get())};
    GMimePtr<GMimeMessage> message{g_mime_parser_construct_message(parser.get(), nullptr)};
    if (!message) {
        return parse_failure("Cannot parse message");
    }

    // 1. 优先选择纯文本，找不到纯文本时选择 HTML
    GMimeObject* part = g_mime_message_get_mime_part(message.get());

    std::size_t count = 0;
    if (auto status = validate_mime_structure(part, 1, count); !status) {
        return std::unexpected(status.error());
    }

    auto text_parts = select_text_parts(part, "plain");
    const bool is_html = text_parts.empty();

    if (is_html) {
        text_parts = select_text_parts(part, "html");
    }
    if (text_parts.empty()) {
        return parse_failure("Expected a text/plain or text/html message");
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

    read_message_headers(message.get(), result);

    if (auto status = collect_attachments(part, result.attachments); !status) {
        return std::unexpected(status.error());
    }

    return result;
}

}  // namespace mailai

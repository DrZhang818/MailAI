#include "mailai/mime.h"

#include <gmime/gmime.h>

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

#include "html_extract.h"
#include "mailai/error_message.h"

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

struct BodyCandidate {
    std::vector<BodyBlock> blocks;
    bool has_body{};
    bool has_plain{};
    bool has_table{};
};

Result<BodyCandidate> read_html_candidate(
    GMimeTextPart* part, mime_detail::PresentationBudget& budget) {
    auto html = decode_text_part(part);
    if (!html) {
        return std::unexpected(html.error());
    }
    auto extracted = mime_detail::html_to_blocks(*html, budget);
    if (!extracted) {
        return std::unexpected(extracted.error());
    }
    return BodyCandidate{
        .blocks = std::move(extracted->blocks), .has_body = true, .has_table = extracted->has_table
    };
}

Result<BodyCandidate> select_body_candidate(
    GMimeObject* part, mime_detail::PresentationBudget& budget) {
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
            auto text = mime_detail::html_to_text(*body);
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

    mime_detail::PresentationBudget budget;
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

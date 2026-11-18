#include "mailai/mime.h"

#include <gmime/gmime.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace mailai {
namespace {

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

GMimeTextPart* as_plain_text_part(GMimeObject* part) {
    if (part == nullptr || !GMIME_IS_TEXT_PART(part)) {
        return nullptr;
    }

    GMimeContentType* type = g_mime_object_get_content_type(part);
    if (!g_mime_content_type_is_type(type, "text", "plain")) {
        return nullptr;
    }

    return GMIME_TEXT_PART(part);
}

bool is_attachment(GMimeObject* part) {
    GMimeContentDisposition* disposition = g_mime_object_get_content_disposition(part);
    return disposition != nullptr && g_mime_content_disposition_is_attachment(disposition);
}

std::vector<GMimeTextPart*> select_plain_text_parts(GMimeObject* part) {
    if (part == nullptr || is_attachment(part)) {
        return {};
    }
    if (GMimeTextPart* text_part = as_plain_text_part(part)) {
        return {text_part};
    }
    if (!GMIME_IS_MULTIPART(part)) {
        return {};
    }

    GMimeMultipart* multipart = GMIME_MULTIPART(part);
    const int count = g_mime_multipart_get_count(multipart);
    GMimeContentType* type = g_mime_object_get_content_type(part);
    const bool is_alternative = g_mime_content_type_is_type(type, "multipart", "alternative");
    const bool is_mixed = g_mime_content_type_is_type(type, "multipart", "mixed");

    std::vector<GMimeTextPart*> text_parts;
    for (int i = 0; i < count; ++i) {
        const int index = is_alternative ? count - 1 - i : i;
        GMimeObject* child = g_mime_multipart_get_part(multipart, index);
        GMimeTextPart* text_part = as_plain_text_part(child);
        if (text_part == nullptr || is_attachment(child)) {
            continue;
        }

        text_parts.push_back(text_part);
        if (!is_mixed) {
            break;
        }
    }
    return text_parts;
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
    if (const char* in_reply_to = g_mime_object_get_header(message_object, "In-Reply-to")) {
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

    static std::once_flag gmime_init_flag;
    std::call_once(gmime_init_flag, g_mime_init);

    GMimePtr<GMimeStream> stream{g_mime_stream_mem_new_with_buffer(raw.data(), raw.size())};
    GMimePtr<GMimeParser> parser{g_mime_parser_new_with_stream(stream.get())};
    GMimePtr<GMimeMessage> message{g_mime_parser_construct_message(parser.get(), nullptr)};
    if (!message) {
        return parse_failure("Cannot parse message");
    }

    GMimeObject* part = g_mime_message_get_mime_part(message.get());
    const auto text_parts = select_plain_text_parts(part);
    if (text_parts.empty()) {
        return parse_failure("Expected a text/plain message");
    }

    ParsedMail result;
    for (std::size_t i = 0; i < text_parts.size(); ++i) {
        std::unique_ptr<char, decltype(&g_free)> body{
            g_mime_text_part_get_text(text_parts[i]), &g_free
        };
        if (!body || !g_utf8_validate(body.get(), -1, nullptr)) {
            return parse_failure("Cannot read a UTF-8 body");
        }

        if (i != 0) {
            result.body.push_back('\n');
        }
        result.body += body.get();
    }

    read_message_headers(message.get(), result);
    return result;
}

}  // namespace mailai

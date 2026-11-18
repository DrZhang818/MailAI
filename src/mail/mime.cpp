#include "mailai/mime.h"

#include <gmime/gmime.h>

#include <memory>
#include <mutex>

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

} // namespace

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
    if (part == nullptr || !GMIME_IS_TEXT_PART(part) ||
        !g_mime_content_type_is_type(g_mime_object_get_content_type(part), "text", "plain")) {
        return parse_failure("Expected a text/plain message");
    }

    std::unique_ptr<char, decltype(&g_free)> body{
        g_mime_text_part_get_text(GMIME_TEXT_PART(part)),
        &g_free
    };
    if (!body || !g_utf8_validate(body.get(), -1, nullptr)) {
        return parse_failure("Cannot read a UTF-8 body");
    }

    ParsedMail result;
    if (const char* subject = g_mime_message_get_subject(message.get())) {
        result.subject = subject;
    }
    result.body = body.get();
    return result;
}

} // namespace mailai
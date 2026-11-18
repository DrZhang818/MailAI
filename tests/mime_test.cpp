#include "mailai/mime.h"

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

mailai::ParsedMail parse_sample(const char* filename) {
    std::ifstream input(filename, std::ios::binary);
    expect(input.is_open(), "could not open sample");
    const std::string raw{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    auto result = mailai::parse_message(raw);
    if (!result) {
        throw std::runtime_error("expected successful MIME parsing: " + result.error().message);
    }
    return std::move(*result);
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
    } catch (const std::exception& error) {
        std::cerr << current_sample << ": " << error.what() << '\n';
        return 1;
    }
}

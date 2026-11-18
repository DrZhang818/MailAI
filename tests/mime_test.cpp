#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include "mailai/mime.h"
#include "test_support.h"

namespace {

void expect_plain_message(const char* filename, std::string_view expected_body) {
    std::ifstream input(filename, std::ios::binary);
    expect(input.is_open(), "could not open sample");
    const std::string raw{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    const auto result = mailai::parse_message(raw);
    if (!result) {
        throw std::runtime_error("expected successful MIME parsing: " + result.error().message);
    }
    expect(result->subject == "会议提醒", "subject should be decoded as UTF-8");
    expect(result->body == expected_body, "plain-text body should match the expected UTF-8 bytes");
}

}  // namespace

int main() {
    const char* current_sample = "plain_utf8.eml";
    try {
        expect_plain_message(current_sample, "请在下午三点参加会议。\n");

        current_sample = "plain_gb18030_base64.eml";
        expect_plain_message(current_sample, "请在下午三点参加会议。");
    } catch (const std::exception& error) {
        std::cerr << current_sample << ": " << error.what() << '\n';
        return 1;
    }
}

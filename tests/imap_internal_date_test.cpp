#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>

#include "mail/imap_protocol.h"
#include "mailai/error_messages.h"
#include "test_support.h"

namespace {

const mailai::AccountId account_id = "internal-date-test";

void expect_date(std::string_view date, std::int64_t expected) {
    const auto result = mailai::imap_detail::parse_internal_date(date, account_id);
    expect(result.has_value(), "Valid INTERNALDATE should be parsed");
    expect(*result == expected, "Incorrect UTC epoch seconds");
}

void expect_invalid_date(std::string_view date) {
    const auto result = mailai::imap_detail::parse_internal_date(date, account_id);
    expect(!result, "Invalid INTERNALDATE should be rejected");
    const auto& error = result.error();
    expect(error.code == mailai::ErrorCode::protocol_error, "Expected protocol_error");
    expect(error.account_id == account_id && error.stage == "imap.fetch",
        "Error should identify the account and stage");
    expect(!error.mail_id && error.message == mailai::error_messages::imap::invalid_internal_date,
        "Error should use the fixed message without the raw response");
}

}  // 匿名命名空间

int main() {
    try {
        expect_date("21-Nov-2026 10:00:00 +0800", 1795226400);
        expect_date("21-nOv-2026 10:00:00 -0530", 1795275000);
        expect_date(" 1-Jan-1970 00:00:00 +0000", 0);
        expect_date("01-JAN-1970 00:00:00 +0030", -1800);
        expect_date("31-Dec-1969 23:59:59 +0000", -1);
        expect_date("31-Dec-1969 23:30:00 -0030", 0);
        expect_date("29-Feb-2000 00:00:00 +0000", 951782400);
        expect_date("01-Jan-0001 00:00:00 +0000", -62135596800LL);
        expect_date("31-Dec-9999 23:59:59 +0000", 253402300799LL);
        expect_date("01-Jan-1970 23:59:00 +2359", 0);

        expect_invalid_date("");
        expect_invalid_date("\"21-Nov-2026 10:00:00 +0800\"");
        expect_invalid_date("21-Nov-2026 10:00:00 +0800 ");
        expect_invalid_date("1-Nov-2026 10:00:00 +0800");
        expect_invalid_date("21/Nov-2026 10:00:00 +0800");
        expect_invalid_date("21-Nov-2026T10:00:00 +0800");
        expect_invalid_date("21-Nov-2026 10-00:00 +0800");
        expect_invalid_date("21-Nov-2026 10:00:00x+0800");
        expect_invalid_date("21-Nov-2026 10:00:00 x0800");
        expect_invalid_date("21-Nov-2026 10:00:00 +08:0");
        expect_invalid_date("21-Nov-2026 1x:00:00 +0800");
        expect_invalid_date("21-xxx-2026 10:00:00 +0800");
        expect_invalid_date("21-Nov-0000 10:00:00 +0800");
        expect_invalid_date("00-Nov-2026 10:00:00 +0800");
        expect_invalid_date("31-Nov-2026 10:00:00 +0800");
        expect_invalid_date("29-Feb-1900 10:00:00 +0800");
        expect_invalid_date("21-Nov-2026 24:00:00 +0800");
        expect_invalid_date("21-Nov-2026 10:60:00 +0800");
        expect_invalid_date("21-Nov-2026 10:00:60 +0800");
        expect_invalid_date("21-Nov-2026 10:00:00 +2400");
        expect_invalid_date("21-Nov-2026 10:00:00 -0860");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

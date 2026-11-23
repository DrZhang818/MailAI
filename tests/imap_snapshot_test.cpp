#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "mail/imap_protocol.h"
#include "test_support.h"

namespace {

const mailai::AccountId account_id = "snapshot-test";

void expect_snapshot(
    std::string_view response, std::uint32_t uid_validity, const std::vector<std::uint32_t>& uids) {
    const auto result = mailai::imap_detail::parse_mailbox_snapshot(response, account_id);
    expect(result.has_value(), "valid mailbox response should be parsed");
    expect(result->uid_validity == uid_validity, "unexpected UIDVALIDITY");
    expect(result->uids == uids, "UIDs should be sorted and deduplicated");
}

void expect_protocol_error(std::string_view response) {
    const auto result = mailai::imap_detail::parse_mailbox_snapshot(response, account_id);
    expect(!result, "invalid mailbox response should be rejected");
    expect(result.error().code == mailai::ErrorCode::protocol_error, "expected protocol_error");
    expect(result.error().account_id == account_id && result.error().stage == "imap.snapshot",
        "error should identify the account and stage");
    expect(!result.error().mail_id &&
               result.error().message.find("private-response-token") == std::string::npos,
        "error should not include the raw response or a mail ID");
}

}  // 匿名命名空间

int main() {
    try {
        expect_snapshot(
            "* FLAGS (\\Seen)\r\n* OK [UIDVALIDITY 77] UIDs valid\r\n"
            "* SEARCH 103 101 101\r\nA001 OK Search completed\r\n",
            77,
            {101, 103});
        expect_snapshot(
            "* oK [UidValidity 4294967295] valid\n"
            "* OK [UIDVALIDITY 4294967295] valid\n* sEaRcH 4294967295 1 1",
            UINT32_MAX,
            {1, UINT32_MAX});
        expect_snapshot("A001 OK [UIDVALIDITY 2] Selected\r\n* SEARCH\r\n", 2, {});
        expect_snapshot(
            "* OK [UIDVALIDITY 1] valid\r\n"
            "* OK note [UIDVALIDITY 999]\r\n+ OK [UIDVALIDITY 888]\r\n"
            "* SEARCHING 1000\r\n* SEARCH 9\r\n",
            1,
            {9});

        expect_protocol_error("");
        expect_protocol_error("* OK [UIDVALIDITY 77]\n");
        expect_protocol_error("* SEARCH\n");
        expect_protocol_error("* OK [UIDVALIDITY 0]\n* SEARCH\n");
        expect_protocol_error("* OK [UIDVALIDITY 4294967296]\n* SEARCH\n");
        expect_protocol_error("* OK [UIDVALIDITY 77oops]\n* SEARCH\n");
        expect_protocol_error("* OK [UIDVALIDITY 77 88]\n* SEARCH\n");
        expect_protocol_error("* OK [UIDVALIDITY 77\n* SEARCH\n");
        expect_protocol_error("* OK [UIDVALIDITY 77]oops\n* SEARCH\n");
        expect_protocol_error("* OK [UIDVALIDITY 77]\n* OK [UIDVALIDITY 78]\n* SEARCH\n");
        expect_protocol_error("* OK [UIDVALIDITY 77]\n* OK [UIDVALIDITY]\n* SEARCH\n");
        expect_protocol_error("* OK [UIDVALIDITY 77]\n* SEARCH 1 0\n");
        expect_protocol_error("* OK [UIDVALIDITY 77]\n* SEARCH 4294967296\n");
        expect_protocol_error("* OK [UIDVALIDITY 77]\n* SEARCH -1\n");
        expect_protocol_error("* OK [UIDVALIDITY 77]\n* SEARCH 1 private-response-token\n");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

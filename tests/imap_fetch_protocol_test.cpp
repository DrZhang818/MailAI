#include <cstddef>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>

#include "mail/imap_protocol.h"
#include "mailai/error_messages.h"
#include "test_support.h"

namespace {

using mailai::ErrorCode;
using mailai::imap_detail::FetchTransfer;

const mailai::RemoteKey key{"protocol-test", "INBOX", 77, 101};
constexpr std::string_view prefix =
    "* 1 FETCH (UID 101 INTERNALDATE \"21-Nov-2026 10:00:00 +0800\" BODY[] {5}\r\n";
constexpr std::string_view body_first = "* 1 FETCH (BODY[] {5}\r\n";
constexpr std::string_view suffix = ")\r\n";
constexpr std::string_view complete = "A004 OK UID FETCH completed\r\n";
constexpr std::string_view raw{"ab\0cd", 5};

void select_inbox(FetchTransfer& transfer) {
    expect(transfer.append_header("* 1 EXISTS\r\n"), "EXISTS should be accepted");
    expect(transfer.append_header("* OK [UIDVALIDITY 77] valid\r\n"), "UIDVALIDITY should match");
    expect(transfer.append_header("A003 OK SELECT completed\r\n"), "SELECT should succeed");
}

void feed(FetchTransfer& transfer,
    std::string_view header,
    std::string_view body,
    std::string_view tail) {
    expect(transfer.append_header(header), "Valid literal prefix should be accepted");
    expect(transfer.append_body(header), "Write callback prefix should be accepted");
    expect(transfer.append_body(body), "Body bytes should be accepted");
    expect(transfer.append_header(tail), "Tail should be buffered");
    expect(transfer.append_header(complete), "Completion should be buffered");
}

void expect_mail(const mailai::Result<mailai::FetchedMail>& result) {
    expect(result.has_value(), "Complete FETCH should be parsed");
    expect(result->raw == raw, "Raw message should preserve every byte");
    expect(result->received_at_utc == 1795226400, "Unexpected UTC timestamp");
    expect(result->key.account_id == key.account_id && result->key.folder == key.folder &&
               result->key.uid_validity == key.uid_validity && result->key.uid == key.uid,
        "Remote identity should be preserved");
}

void expect_error(const mailai::Result<mailai::FetchedMail>& result, ErrorCode code) {
    expect(!result && result.error().code == code, "Unexpected FETCH error code");
    expect(result.error().account_id == key.account_id && result.error().stage == "imap.fetch" &&
               !result.error().mail_id,
        "Error context should identify the account and stage");
}

void test_literal_layouts() {
    FetchTransfer normal(key);
    select_inbox(normal);
    feed(normal, prefix, raw, suffix);
    expect_mail(normal.finish());

    FetchTransfer reversed(key);
    select_inbox(reversed);
    feed(reversed, body_first, raw, " UID 101 INTERNALDATE \"21-Nov-2026 10:00:00 +0800\")\r\n");
    expect_mail(reversed.finish());

    FetchTransfer extra_fields(key);
    select_inbox(extra_fields);
    feed(extra_fields,
        "* 1 FETCH (FLAGS (\\Seen) RFC822.SIZE 5 MODSEQ (42) "
        "INTERNALDATE \"21-Nov-2026 10:00:00 +0800\" UID 101 BODY[] {5}\r\n",
        raw,
        suffix);
    expect_mail(extra_fields.finish());

    // 同一个原文在不同位置分片，并测试协议响应占用写回调字节数的布局。
    for (std::size_t split = 0; split <= raw.size(); ++split) {
        FetchTransfer overflow(key);
        select_inbox(overflow);
        constexpr std::string_view flags = "* 1 FETCH (UID 101 FLAGS ())\r\n";
        expect(overflow.append_header(flags) && overflow.append_body(flags), "FLAGS response");
        expect(overflow.append_header(prefix), "Literal prefix");
        expect(
            overflow.append_body(prefix.substr(0, 12)) && overflow.append_body(prefix.substr(12)),
            "Fragmented prefix");
        expect(overflow.append_body(raw.substr(0, split)), "Initial literal bytes");
        expect(overflow.append_header(raw.substr(split)), "Remaining literal bytes");
        expect(overflow.append_header(suffix) && overflow.append_header(complete), "Completion");
        expect_mail(overflow.finish());
    }
}

void test_invalid_responses() {
    for (const auto tail :
        {std::string_view{},
            std::string_view{" UID 101)\r\n"},
            std::string_view{" INTERNALDATE \"21-Nov-2026 10:00:00 +0800\")\r\n"}}) {
        FetchTransfer invalid(key);
        select_inbox(invalid);
        feed(invalid, prefix, raw, tail);
        expect_error(invalid.finish(), ErrorCode::protocol_error);
    }
    for (const auto value : {raw.substr(0, 2), std::string_view{"123456"}}) {
        FetchTransfer invalid(key);
        select_inbox(invalid);
        feed(invalid, prefix, value, suffix);
        expect_error(invalid.finish(), ErrorCode::protocol_error);
    }
    for (const auto invalid_prefix :
        {"* 1 FETCH (UID 102 INTERNALDATE \"21-Nov-2026 10:00:00 +0800\" BODY[] {5}\r\n",
            "* 1 FETCH (UID 101 BODY[] {5}\r\n",
            "* 1 FETCH (UID 101 INTERNALDATE \"31-Nov-2026 10:00:00 +0800\" BODY[] {5}\r\n"}) {
        FetchTransfer invalid(key);
        select_inbox(invalid);
        feed(invalid, invalid_prefix, raw, suffix);
        expect_error(invalid.finish(), ErrorCode::protocol_error);
    }
    FetchTransfer oversized(key);
    select_inbox(oversized);
    expect(!oversized.append_header("* 1 FETCH (UID 101 BODY[] {20971521}\r\n"),
        "Reject oversized literal");
    expect_error(oversized.finish(), ErrorCode::protocol_error);
    expect(oversized.finish().error().message == mailai::error_messages::imap::fetch_too_large,
        "Size error should preserve its diagnostic");
}

void test_error_precedence() {
    const mailai::Error transport_error{
        .code = ErrorCode::network_error,
        .message = "Synthetic connection failure",
        .account_id = key.account_id,
        .mail_id = std::nullopt,
        .stage = "imap.fetch"
    };

    FetchTransfer missing(key);
    select_inbox(missing);
    expect(missing.append_header(complete), "No matching mail");
    expect_error(missing.finish(), ErrorCode::remote_gone);
    expect_error(missing.finish(transport_error), ErrorCode::network_error);

    FetchTransfer fully_received(key);
    select_inbox(fully_received);
    feed(fully_received, prefix, raw, suffix);
    const auto failed_transfer = fully_received.finish(transport_error);
    expect_error(failed_transfer, ErrorCode::network_error);
    expect(failed_transfer.error().message == transport_error.message,
        "Transport failure should preserve its original diagnostic");

    FetchTransfer partial(key);
    select_inbox(partial);
    expect(partial.append_header(prefix) && partial.append_body(prefix) &&
               partial.append_body(raw.substr(0, 2)),
        "Buffer incomplete literal before transport interruption");
    expect_error(partial.finish(transport_error), ErrorCode::network_error);

    FetchTransfer stale(key);
    expect(!stale.append_header("* OK [UIDVALIDITY 78] valid\r\n"), "Reject stale UIDVALIDITY");
    expect_error(stale.finish(transport_error), ErrorCode::remote_gone);

    FetchTransfer unavailable(key);
    select_inbox(unavailable);
    expect(
        !unavailable.append_header("A004 NO [UNAVAILABLE] Retry later\r\n"), "Service unavailable");
    expect_error(unavailable.finish(), ErrorCode::network_error);

    FetchTransfer late_unavailable(key);
    select_inbox(late_unavailable);
    expect(late_unavailable.append_header(prefix) && late_unavailable.append_body(prefix) &&
               late_unavailable.append_body(raw) && late_unavailable.append_header(suffix) &&
               late_unavailable.append_header("A004 NO [UNAVAILABLE] Retry later\r\n"),
        "Temporary failure after complete literal");
    expect_error(late_unavailable.finish(), ErrorCode::network_error);
    auto generic_protocol = transport_error;
    generic_protocol.code = ErrorCode::protocol_error;
    const auto temporary_failure = late_unavailable.finish(generic_protocol);
    expect_error(temporary_failure, ErrorCode::network_error);
    expect(temporary_failure.error().message == mailai::error_messages::imap::service_unavailable,
        "Explicit service response should take precedence over a generic transport error");

    FetchTransfer aborted(key);
    select_inbox(aborted);
    aborted.abort();
    expect(!aborted.append_body(raw), "Do not accept data after a callback failure");
    expect_error(aborted.finish(transport_error), ErrorCode::protocol_error);

    using mailai::imap_detail::has_service_unavailable;
    expect(has_service_unavailable("A004 NO [UNAVAILABLE] Retry later\r\n"), "Temporary rejection");
    expect(has_service_unavailable("* BYE [UNAVAILABLE] Retry later\r\n"), "Temporary disconnect");
    expect(!has_service_unavailable("A004 NO Search denied [UNAVAILABLE]\r\n"),
        "Ordinary diagnostic text is not a response code");
}

}  // 匿名命名空间

int main() {
    try {
        test_literal_layouts();
        test_invalid_responses();
        test_error_precedence();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

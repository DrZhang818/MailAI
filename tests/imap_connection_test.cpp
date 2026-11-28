#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mail/imap_transport.h"
#include "mailai/error_message.h"
#include "mailai/imap.h"
#include "test_support.h"

namespace {

std::size_t discard_response(char*, std::size_t size, std::size_t count, void*) {
    return size * count;
}

void expect_config_failure(const mailai::RuntimeConfig& config, std::string_view path = "/") {
    const auto result = mailai::imap_detail::make_imap_handle(config, path);
    expect(!result, "invalid IMAP configuration should be rejected");
    expect(result.error().code == mailai::ErrorCode::config_error, "expected config_error");
    expect(result.error().account_id == config.account.id, "error should identify the account");
    expect(result.error().stage == "imap.connect", "expected IMAP connection stage");
    expect(result.error().message.find("fixture-only-password") == std::string::npos,
        "error should not include the test password");
}

void expect_invalid_settings(const mailai::RuntimeConfig& good) {
    auto invalid = good;
    invalid.imap.port = 0;
    expect_config_failure(invalid);
    invalid = good;
    invalid.imap.host.clear();
    expect_config_failure(invalid);
    invalid = good;
    invalid.imap.host = "invalid host";
    expect_config_failure(invalid);
    invalid = good;
    invalid.imap.username = std::string{"a\0b", 3};
    expect_config_failure(invalid);
    invalid = good;
    invalid.imap.total_timeout = std::chrono::milliseconds{0};
    expect_config_failure(invalid);
    invalid = good;
    invalid.imap.tls_mode = static_cast<mailai::TlsMode>(100);
    expect_config_failure(invalid);
    invalid = good;
    invalid.imap.ca_file = std::filesystem::path{};
    expect_config_failure(invalid);
    expect_config_failure(good, "");
    expect_config_failure(good, "INBOX");
    expect_config_failure(good, std::string_view{"/INBOX\0tail", 11});
}

void expect_snapshot_query(const mailai::RuntimeConfig& config, std::string_view scenario) {
    const auto snapshot = mailai::read_mailbox_snapshot(config);
    if (scenario == "ok" || scenario == "empty" || scenario == "timeout-recover" ||
        scenario == "provider") {
        expect(snapshot.has_value(), "valid inbox query should return a snapshot");
        expect(snapshot->uid_validity == 77, "unexpected server UIDVALIDITY");
        const std::vector<std::uint32_t> expected =
            scenario == "empty" ? std::vector<std::uint32_t>{} : std::vector<std::uint32_t>{101};
        expect(snapshot->uids == expected, "unexpected server UID list");
        return;
    }

    mailai::ErrorCode expected = mailai::ErrorCode::protocol_error;
    if (scenario == "wrong-password") {
        expected = mailai::ErrorCode::auth_error;
    } else if (scenario == "untrusted" || scenario == "wrong-hostname" ||
               scenario == "missing-password" || scenario == "no-starttls") {
        expected = mailai::ErrorCode::config_error;
    } else if (scenario == "timeout" || scenario == "network-error") {
        expected = mailai::ErrorCode::network_error;
    }
    expect(!snapshot && snapshot.error().code == expected, "unexpected inbox query error type");
    expect(snapshot.error().account_id == config.account.id, "error should identify the account");
    expect(snapshot.error().stage ==
               (scenario == "missing-password" ? "imap.connect" : "imap.snapshot"),
        "unexpected inbox query error stage");
    expect(snapshot.error().message.find("fixture-only-password") == std::string::npos &&
               snapshot.error().message.find("wrong-test-password") == std::string::npos,
        "error should not include the test password");
}

void expect_fetch(const mailai::RuntimeConfig& config, std::string_view scenario) {
    mailai::RemoteKey key{config.account.id, "INBOX", 77, 101};
    if (scenario == "invalid-account")
        key.account_id = "other-account";
    if (scenario == "invalid-folder")
        key.folder = "Sent";
    if (scenario == "invalid-uid")
        key.uid = 0;
    if (scenario == "invalid-validity")
        key.uid_validity = 0;
    if (scenario == "missing-uid")
        key.uid = 102;

    const auto result = mailai::fetch_mail(config, key);
    if (scenario == "ok" || scenario == "provider" || scenario == "timeout-recover" ||
        scenario == "body-first" || scenario == "flags-before" || scenario == "binary" ||
        scenario == "no-final-crlf" || scenario == "at-limit") {
        expect(result.has_value(), "valid UID FETCH should return a mail");
        expect(result->key.account_id == key.account_id && result->key.folder == key.folder &&
                   result->key.uid_validity == key.uid_validity && result->key.uid == key.uid,
            "fetched mail should preserve its remote key");
        expect(result->received_at_utc == 1795226400, "unexpected INTERNALDATE timestamp");
        std::string expected =
            "From: sender@example.test\r\nSubject: IMAP fixture\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n\r\n这是一封测试邮件。\r\n";
        if (scenario == "no-final-crlf")
            expected.resize(expected.size() - 2);
        if (scenario == "binary")
            expected.append("\x00\xff\x01\r\n", 5);
        if (scenario == "at-limit")
            expected.resize(20 * 1024 * 1024, 'a');
        expect(result->raw == expected, "fetched raw bytes should match the original exactly");
        return;
    }

    auto expected = mailai::ErrorCode::protocol_error;
    if (scenario.starts_with("invalid-") && scenario != "invalid-date") {
        expected = mailai::ErrorCode::invalid_argument;
    } else if (scenario == "stale" || scenario == "missing-uid" || scenario == "empty-mailbox") {
        expected = mailai::ErrorCode::remote_gone;
    } else if (scenario == "wrong-password") {
        expected = mailai::ErrorCode::auth_error;
    } else if (scenario == "untrusted" || scenario == "missing-password" ||
               scenario == "wrong-hostname" || scenario == "no-starttls") {
        expected = mailai::ErrorCode::config_error;
    } else if (scenario == "timeout" || scenario == "network-error") {
        expected = mailai::ErrorCode::network_error;
    }
    expect(!result && result.error().code == expected, "unexpected UID FETCH error type");
    if (scenario == "over-limit") {
        expect(result.error().message == mailai::error_message::imap::fetch_too_large,
            "oversized literal should be rejected by the size guard");
    }
    expect(result.error().account_id == config.account.id, "error should identify the account");
    expect(result.error().stage == (scenario == "missing-password" ? "imap.connect" : "imap.fetch"),
        "unexpected UID FETCH error stage");
    expect(result.error().message.find("fixture-only-password") == std::string::npos &&
               result.error().message.find("wrong-test-password") == std::string::npos &&
               result.error().message.find("From: sender@example.test") == std::string::npos &&
               result.error().message.find("这是一封测试邮件。") == std::string::npos,
        "error should not include the test password or message contents");
}

void expect_session(mailai::RuntimeConfig config, std::string_view scenario) {
    const bool idle_disconnect = scenario.starts_with("idle-");
    int credential_requests = 0;
    if (idle_disconnect) {
        config.credential_provider = [&] {
            ++credential_requests;
            return std::string{"fixture-only-password"};
        };
    }
    mailai::ImapSession session{config};
    const auto snapshot = session.read_mailbox_snapshot();
    expect(snapshot.has_value() && snapshot->uid_validity == 77,
        "session snapshot should succeed with UIDVALIDITY 77");

    const mailai::RemoteKey present{config.account.id, "INBOX", 77, 101};
    const mailai::RemoteKey missing{config.account.id, "INBOX", 77, 102};

    if (idle_disconnect) {
        // 等待服务端关闭快照使用的连接，再发起 FETCH。
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        const auto result = session.fetch_mail(present);
        // libcurl 可以复用原句柄重连，应用层也可以重试并重建一次句柄。
        expect(credential_requests >= 1 && credential_requests <= 2,
            "idle reconnect should use the original easy handle or rebuild it once");
        if (scenario == "idle-reconnect") {
            expect(result.has_value(), "reconnected session must accept matching UIDVALIDITY");
        } else if (scenario == "idle-reconnect-stale") {
            expect(!result && result.error().code == mailai::ErrorCode::remote_gone &&
                       result.error().message == mailai::error_message::imap::uid_validity_changed,
                "reconnected session must reject changed UIDVALIDITY");
        } else {
            expect(!result && result.error().code == mailai::ErrorCode::protocol_error &&
                       result.error().message == mailai::error_message::imap::missing_uid_validity,
                "reconnected session must reject missing UIDVALIDITY");
        }
        return;
    }

    if (scenario == "reuse") {
        const auto first = session.fetch_mail(present);
        expect(first.has_value() && first->key.uid == 101, "first session fetch should succeed");
        const auto gone = session.fetch_mail(missing);
        expect(!gone && gone.error().code == mailai::ErrorCode::remote_gone,
            "missing UID should return remote_gone");
        const auto second = session.fetch_mail(present);
        expect(second.has_value() && second->raw == first->raw,
            "session fetch after remote_gone should reuse the connection");
        return;
    }

    if (scenario == "reconnect") {
        const auto recovered = session.fetch_mail(present);
        expect(recovered.has_value(), "session fetch should recover after disconnect");
        const auto reused = session.fetch_mail(present);
        expect(reused.has_value() && reused->raw == recovered->raw,
            "session should reuse the reconnected handle");
        return;
    }

    if (scenario == "stale-reconnect") {
        const auto stale = session.fetch_mail(present);
        expect(!stale && stale.error().code == mailai::ErrorCode::remote_gone,
            "stale UIDVALIDITY on reconnect should return remote_gone");
        return;
    }

    throw std::runtime_error("unknown session scenario");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        expect(argc == 5, "expected fixture port, CA, TLS mode and scenario");
        const std::string_view operation{argv[4]};
        constexpr std::string_view snapshot_prefix = "snapshot-";
        constexpr std::string_view fetch_prefix = "fetch-";
        constexpr std::string_view session_prefix = "session-";
        const bool read_snapshot = operation.starts_with(snapshot_prefix);
        const bool fetch = operation.starts_with(fetch_prefix);
        const bool session = operation.starts_with(session_prefix);
        const auto scenario = read_snapshot ? operation.substr(snapshot_prefix.size())
                              : fetch       ? operation.substr(fetch_prefix.size())
                              : session     ? operation.substr(session_prefix.size())
                                            : operation;
        mailai::RuntimeConfig config;
        config.account.id = "imap-fixture";
        config.imap.host = "127.0.0.1";
        config.imap.port = static_cast<std::uint16_t>(std::stoul(argv[1]));
        config.imap.username = "mailai-test";
        config.imap.credential_env = "MAILAI_FIXTURE_PASSWORD";
        if (scenario == "provider")
            config.credential_provider = [] { return std::string{"fixture-only-password"}; };
        config.imap.tls_mode = std::string_view{argv[3]} == "implicit" ? mailai::TlsMode::implicit
                                                                       : mailai::TlsMode::starttls;
        config.imap.connect_timeout = std::chrono::seconds{2};
        config.imap.total_timeout = std::chrono::seconds{5};
        if (scenario != "untrusted") {
            config.imap.ca_file = argv[2];
        }
        if (session) {
            expect_session(config, scenario);
            return 0;
        }
        if (fetch) {
            if (scenario.starts_with("timeout"))
                config.imap.total_timeout = std::chrono::seconds{1};
            expect_fetch(config, scenario);
            return 0;
        }
        if (read_snapshot) {
            if (scenario.starts_with("timeout")) {
                config.imap.total_timeout = std::chrono::seconds{1};
            }
            expect_snapshot_query(config, scenario);
            return 0;
        }
        if (scenario == "missing-password") {
            expect_config_failure(config);
            return 0;
        }
        if (scenario == "ok") {
            expect_invalid_settings(config);
        }

        auto handle = mailai::imap_detail::make_imap_handle(config);
        expect(handle.has_value(), "valid IMAP configuration should produce a handle");
        expect(curl_easy_setopt(handle->get(), CURLOPT_CUSTOMREQUEST, "NOOP") == CURLE_OK,
            "setting NOOP failed");
        expect(
            curl_easy_setopt(handle->get(), CURLOPT_WRITEFUNCTION, &discard_response) == CURLE_OK,
            "setting response callback failed");
        const auto status = curl_easy_perform(handle->get());
        if (scenario == "wrong-password") {
            expect(status == CURLE_LOGIN_DENIED, "incorrect password should be rejected");
        } else if (scenario == "untrusted" || scenario == "wrong-hostname") {
            expect(
                status == CURLE_PEER_FAILED_VERIFICATION, "invalid certificate should be rejected");
        } else {
            if (status != CURLE_OK) {
                throw std::runtime_error(
                    std::string{"IMAP request failed: "} + curl_easy_strerror(status));
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

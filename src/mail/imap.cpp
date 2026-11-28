#include "mailai/imap.h"

#include <curl/curl.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "imap_protocol.h"
#include "imap_transport.h"
#include "mailai/error_message.h"

namespace mailai {
namespace {
namespace errors = error_message::imap;

// 公共连接、错误分类和重试工具。
struct CurlGlobal {
    CURLcode status{curl_global_init(CURL_GLOBAL_DEFAULT)};

    ~CurlGlobal() {
        if (status == CURLE_OK) {
            curl_global_cleanup();
        }
    }
};

struct CurlOptionSetter {
    CURL* handle;
    CURLcode code{CURLE_OK};

    bool operator()(CURLoption option, auto value) {
        code = curl_easy_setopt(handle, option, value);
        return code == CURLE_OK;
    }
};

std::optional<std::size_t> callback_size(std::size_t size, std::size_t count) {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        return std::nullopt;
    }
    return size * count;
}

ErrorCode classify_transfer_error(CURLcode status) {
    switch (status) {
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
        case CURLE_SSL_CONNECT_ERROR:
            return ErrorCode::network_error;
        case CURLE_LOGIN_DENIED:
            return ErrorCode::auth_error;
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CRL_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        case CURLE_SSL_INVALIDCERTSTATUS:
        case CURLE_SSL_CIPHER:
        case CURLE_USE_SSL_FAILED:
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
        case CURLE_NOT_BUILT_IN:
        case CURLE_FAILED_INIT:
        case CURLE_BAD_FUNCTION_ARGUMENT:
            return ErrorCode::config_error;
        case CURLE_QUOTE_ERROR:
        case CURLE_REMOTE_ACCESS_DENIED:
        case CURLE_REMOTE_FILE_NOT_FOUND:
        case CURLE_WEIRD_SERVER_REPLY:
        case CURLE_WRITE_ERROR:
        case CURLE_ABORTED_BY_CALLBACK:
        case CURLE_BAD_CONTENT_ENCODING:
        default:
            return ErrorCode::protocol_error;
    }
}

// 仅重试临时网络或服务故障。每次尝试都重置会话状态并重建句柄。
template <class Operation>
auto retry_imap(Operation operation) {
    constexpr std::array delays{std::chrono::seconds{1}, std::chrono::seconds{2}};
    auto result = operation();
    for (const auto delay : delays) {
        if (result || result.error().code != ErrorCode::network_error) {
            break;
        }
        std::this_thread::sleep_for(delay);
        result = operation();
    }
    return result;
}

bool valid_text(std::string_view value) {
    return !value.empty() && value.find('\0') == std::string_view::npos;
}

Result<std::string> build_imap_url(
    const ImapSettings& imap, const AccountId& account_id, std::string_view mailbox_path) {
    auto fail = [&](std::string message) {
        return std::unexpected(Error{
            .code = ErrorCode::config_error,
            .message = std::move(message),
            .account_id = account_id,
            .mail_id = std::nullopt,
            .stage = errors::connect_stage
        });
    };

    if (!mailbox_path.starts_with('/') || !valid_text(mailbox_path)) {
        return fail(errors::invalid_mailbox_path);
    }

    const char* scheme = imap.tls_mode == TlsMode::implicit ? "imaps" : "imap";
    const std::string port = std::to_string(imap.port);
    const std::string path{mailbox_path};

    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url{curl_url(), &curl_url_cleanup};
    if (!url) {
        return fail(errors::url_creation_failed);
    }

    CURLUcode url_code = CURLUE_OK;
    auto set_url_part = [&](CURLUPart part, const char* value) {
        url_code = curl_url_set(url.get(), part, value, 0);
        return url_code == CURLUE_OK;
    };

    if (!set_url_part(CURLUPART_SCHEME, scheme) ||
        !set_url_part(CURLUPART_HOST, imap.host.c_str()) ||
        !set_url_part(CURLUPART_PORT, port.c_str()) ||
        !set_url_part(CURLUPART_PATH, path.c_str())) {
        return fail(std::string(errors::invalid_server_url) + ": " + curl_url_strerror(url_code));
    }

    char* raw_url = nullptr;
    url_code = curl_url_get(url.get(), CURLUPART_URL, &raw_url, 0);

    std::unique_ptr<char, decltype(&curl_free)> server_url{raw_url, &curl_free};
    if (url_code != CURLUE_OK || !server_url) {
        return fail(errors::url_read_failed);
    }
    return std::string{server_url.get()};
}

Result<imap_detail::CurlHandle> make_imap_handle_impl(
    const RuntimeConfig& config, std::string_view mailbox_path) {
    const auto& imap = config.imap;

    auto fail = [&](std::string message) {
        return std::unexpected(Error{
            .code = ErrorCode::config_error,
            .message = std::move(message),
            .account_id = config.account.id,
            .mail_id = std::nullopt,
            .stage = errors::connect_stage
        });
    };

    // 1. 检查配置并读取密码
    if (!valid_text(config.account.id) || !valid_text(imap.host) || !valid_text(imap.username) ||
        !valid_credential_reference(imap.credential_env)) {
        return fail(errors::invalid_settings);
    }
    if (!mailbox_path.starts_with('/') || !valid_text(mailbox_path)) {
        return fail(errors::invalid_mailbox_path);
    }

    if (imap.port == 0 ||
        (imap.tls_mode != TlsMode::implicit && imap.tls_mode != TlsMode::starttls)) {
        return fail(errors::invalid_port_or_tls);
    }

    const auto connect_ms = imap.connect_timeout.count();
    const auto total_ms = imap.total_timeout.count();

    if (connect_ms <= 0 || total_ms <= 0 || !std::in_range<long>(connect_ms) ||
        !std::in_range<long>(total_ms)) {
        return fail(errors::invalid_timeout);
    }

    const std::string ca_path = imap.ca_file ? imap.ca_file->string() : std::string{};
    if (imap.ca_file && !valid_text(ca_path)) {
        return fail(errors::invalid_ca_path);
    }

    std::string password;
    if (config.credential_provider) {
        auto supplied = config.credential_provider();
        if (!supplied)
            return std::unexpected(supplied.error());
        password = std::move(*supplied);
    } else if (const char* password_value = std::getenv(imap.credential_env.c_str())) {
        password = password_value;
    }
    if (!valid_text(password))
        return fail(errors::missing_password);

    // 2. 初始化 libcurl 和 easy handle
    static const CurlGlobal global;
    if (global.status != CURLE_OK) {
        return fail(
            std::string(errors::libcurl_init_failed) + ": " + curl_easy_strerror(global.status));
    }

    imap_detail::CurlHandle handle{curl_easy_init(), &curl_easy_cleanup};
    if (!handle) {
        return fail(errors::handle_creation_failed);
    }

    CurlOptionSetter set_option{handle.get()};

    auto option_failure = [&](const char* message) {
        return fail(std::string(message) + ": " + curl_easy_strerror(set_option.code));
    };

    // 3. 构造服务器 URL
    auto server_url = build_imap_url(imap, config.account.id, mailbox_path);
    if (!server_url) {
        return std::unexpected(server_url.error());
    }

    if (!set_option(CURLOPT_URL, server_url->c_str())) {
        return option_failure(errors::url_set_failed);
    }

    // 4. 配置认证和 TLS 证书校验
    if (!set_option(CURLOPT_USERNAME, imap.username.c_str()) ||
        !set_option(CURLOPT_PASSWORD, password.c_str()) ||
        !set_option(CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL)) ||
        !set_option(CURLOPT_SSL_VERIFYPEER, 1L) || !set_option(CURLOPT_SSL_VERIFYHOST, 2L) ||
        (imap.ca_file && !set_option(CURLOPT_CAINFO, ca_path.c_str()))) {
        return option_failure(errors::authentication_tls_failed);
    }

    // 5. 设置超时，协议及日志选项
    const char* scheme = imap.tls_mode == TlsMode::implicit ? "imaps" : "imap";
    if (!set_option(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(connect_ms)) ||
        !set_option(CURLOPT_TIMEOUT_MS, static_cast<long>(total_ms)) ||
        !set_option(CURLOPT_PROTOCOLS_STR, scheme) || !set_option(CURLOPT_NOSIGNAL, 1L) ||
        !set_option(CURLOPT_VERBOSE, 0L)) {
        return option_failure(errors::transfer_options_failed);
    }

    // 6. 返回已配置的 handle
    return handle;
}

// 收件箱快照与单封下载的回调函数。
std::size_t append_response(char* data, std::size_t size, std::size_t count, void* userdata) {
    const auto bytes = callback_size(size, count);
    if (!bytes) {
        return CURL_WRITEFUNC_ERROR;
    }
    if (*bytes == 0) {
        return 0;
    }
    if (data == nullptr || userdata == nullptr) {
        return CURL_WRITEFUNC_ERROR;
    }
    try {
        static_cast<std::string*>(userdata)->append(data, *bytes);
    } catch (...) {
        return CURL_WRITEFUNC_ERROR;
    }
    return *bytes;
}

std::size_t discard_response(char*, std::size_t size, std::size_t count, void*) {
    return callback_size(size, count).value_or(CURL_WRITEFUNC_ERROR);
}

std::size_t fetch_header_callback(char* data, std::size_t size, std::size_t count, void* userdata) {
    const auto bytes = callback_size(size, count);
    if (!bytes || !userdata || (!data && *bytes != 0))
        return CURL_WRITEFUNC_ERROR;
    if (*bytes == 0)
        return 0;
    auto& transfer = *static_cast<imap_detail::FetchTransfer*>(userdata);
    try {
        if (transfer.append_header({data, *bytes}))
            return *bytes;
    } catch (...) {
        transfer.abort();
    }
    return CURL_WRITEFUNC_ERROR;
}

std::size_t fetch_body_callback(char* data, std::size_t size, std::size_t count, void* userdata) {
    const auto bytes = callback_size(size, count);
    if (!bytes || !userdata || (!data && *bytes != 0))
        return CURL_WRITEFUNC_ERROR;
    if (*bytes == 0)
        return 0;
    auto& transfer = *static_cast<imap_detail::FetchTransfer*>(userdata);
    try {
        if (transfer.append_body({data, *bytes}))
            return *bytes;
    } catch (...) {
        transfer.abort();
    }
    return CURL_WRITEFUNC_ERROR;
}

// 每次请求结束后清除指向栈上局部对象的回调指针，避免后续清理句柄发 LOGOUT 时悬空访问。
struct CallbackReset {
    CURL* handle;

    ~CallbackReset() {
        if (handle) {
            curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &discard_response);
            curl_easy_setopt(handle, CURLOPT_HEADERDATA, nullptr);
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &discard_response);
            curl_easy_setopt(handle, CURLOPT_WRITEDATA, nullptr);
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, nullptr);
        }
    }
};

}  // namespace

struct ImapSession::State {
    RuntimeConfig config;
    imap_detail::CurlHandle handle{nullptr, &curl_easy_cleanup};
    std::uint32_t selected_uid_validity{0};

    void reset() noexcept {
        handle.reset();
        selected_uid_validity = 0;
    }

    Result<CURL*> ensure_handle(std::string_view mailbox_path) {
        if (!handle) {
            auto created = make_imap_handle_impl(config, mailbox_path);
            if (!created)
                return std::unexpected(created.error());
            handle = std::move(*created);
            selected_uid_validity = 0;
            return handle.get();
        }

        auto url = build_imap_url(config.imap, config.account.id, mailbox_path);
        if (!url) {
            reset();
            return std::unexpected(url.error());
        }
        CurlOptionSetter set_option{handle.get()};
        if (!set_option(CURLOPT_URL, url->c_str())) {
            reset();
            return std::unexpected(Error{
                .code = ErrorCode::config_error,
                .message = std::string(errors::url_set_failed) + ": " +
                           curl_easy_strerror(set_option.code),
                .account_id = config.account.id,
                .mail_id = std::nullopt,
                .stage = errors::connect_stage
            });
        }
        return handle.get();
    }
};

ImapSession::ImapSession(RuntimeConfig config)
    : state_(std::make_unique<State>(State{std::move(config)})) {}

ImapSession::~ImapSession() = default;
ImapSession::ImapSession(ImapSession&& other) noexcept = default;
ImapSession& ImapSession::operator=(ImapSession&& other) noexcept = default;

Result<MailboxSnapshot> ImapSession::read_mailbox_snapshot() {
    if (!state_) {
        return std::unexpected(Error{
            .code = ErrorCode::invalid_argument,
            .message = errors::invalid_settings,
            .account_id = std::nullopt,
            .mail_id = std::nullopt,
            .stage = errors::snapshot_stage
        });
    }

    auto read_once = [&]() -> Result<MailboxSnapshot> {
        auto fail = [&](ErrorCode code, const char* message, CURLcode status) {
            return std::unexpected(Error{
                .code = code,
                .message = std::string(message) + ": " + curl_easy_strerror(status),
                .account_id = state_->config.account.id,
                .mail_id = std::nullopt,
                .stage = errors::snapshot_stage
            });
        };

        // 每次快照都从干净的句柄开始，确保拿到最新的 SELECT [UIDVALIDITY] 响应。
        state_->reset();
        std::string response;
        auto handle = state_->ensure_handle("/INBOX");
        if (!handle) {
            return std::unexpected(handle.error());
        }

        CurlOptionSetter set_option{*handle};
        CURLcode status = CURLE_OK;
        {
            CallbackReset guard{*handle};
            if (set_option(CURLOPT_CUSTOMREQUEST, "UID SEARCH ALL") &&
                set_option(CURLOPT_HEADERFUNCTION, &append_response) &&
                set_option(CURLOPT_HEADERDATA, &response) &&
                set_option(CURLOPT_WRITEFUNCTION, &discard_response)) {
                status = curl_easy_perform(*handle);
            }
        }
        if (set_option.code != CURLE_OK) {
            state_->reset();
            return fail(ErrorCode::config_error, errors::snapshot_options_failed, set_option.code);
        }

        if (status != CURLE_OK) {
            state_->reset();
            if (imap_detail::has_service_unavailable(response)) {
                return fail(ErrorCode::network_error, errors::service_unavailable, status);
            }
            return fail(classify_transfer_error(status), errors::snapshot_request_failed, status);
        }

        auto snapshot = imap_detail::parse_mailbox_snapshot(response, state_->config.account.id);
        if (!snapshot) {
            state_->reset();
            return snapshot;
        }
        state_->selected_uid_validity = snapshot->uid_validity;
        return snapshot;
    };

    return retry_imap(read_once);
}

Result<FetchedMail> ImapSession::fetch_mail(const RemoteKey& key) {
    if (!state_) {
        return std::unexpected(Error{
            .code = ErrorCode::invalid_argument,
            .message = errors::invalid_remote_key,
            .account_id = key.account_id,
            .mail_id = std::nullopt,
            .stage = errors::fetch_stage
        });
    }

    auto make_error = [&](ErrorCode code, std::string message) {
        return Error{
            .code = code,
            .message = std::move(message),
            .account_id = state_->config.account.id,
            .mail_id = std::nullopt,
            .stage = errors::fetch_stage
        };
    };

    // 1. 检查远端邮件标识。
    if (key.account_id.empty() || key.account_id != state_->config.account.id ||
        key.folder != "INBOX" || key.uid_validity == 0 || key.uid == 0) {
        return std::unexpected(make_error(ErrorCode::invalid_argument, errors::invalid_remote_key));
    }

    const auto path = "/INBOX;UIDVALIDITY=" + std::to_string(key.uid_validity);
    const auto command = "UID FETCH " + std::to_string(key.uid) + " (INTERNALDATE BODY.PEEK[])";

    auto fetch_once = [&]() -> Result<FetchedMail> {
        if (state_->selected_uid_validity != 0 &&
            state_->selected_uid_validity != key.uid_validity) {
            state_->reset();
        }
        const bool had_selected =
            state_->handle != nullptr && state_->selected_uid_validity == key.uid_validity;
        const std::uint32_t inherited_validity = had_selected ? state_->selected_uid_validity : 0;

        auto handle = state_->ensure_handle(path);
        if (!handle) {
            return std::unexpected(handle.error());
        }

        imap_detail::FetchTransfer transfer(key, inherited_validity);
        CurlOptionSetter set_option{*handle};
        CURLcode status = CURLE_OK;
        {
            CallbackReset guard{*handle};
            if (set_option(CURLOPT_CUSTOMREQUEST, command.c_str()) &&
                set_option(CURLOPT_HEADERFUNCTION, &fetch_header_callback) &&
                set_option(CURLOPT_HEADERDATA, &transfer) &&
                set_option(CURLOPT_WRITEFUNCTION, &fetch_body_callback) &&
                set_option(CURLOPT_WRITEDATA, &transfer)) {
                status = curl_easy_perform(*handle);
            }
        }
        if (set_option.code != CURLE_OK) {
            state_->reset();
            return std::unexpected(make_error(ErrorCode::config_error,
                std::string(errors::fetch_options_failed) + ": " +
                    curl_easy_strerror(set_option.code)));
        }

        long new_connects = 0;
        // 即使复用同一个 easy handle，libcurl 也可能在请求开始时重新建连。
        const bool require_observed_validity =
            !had_selected ||
            curl_easy_getinfo(*handle, CURLINFO_NUM_CONNECTS, &new_connects) != CURLE_OK ||
            new_connects > 0;

        if (status != CURLE_OK) {
            state_->reset();
            return transfer.finish(
                make_error(classify_transfer_error(status),
                    std::string(errors::fetch_request_failed) + ": " + curl_easy_strerror(status)),
                require_observed_validity);
        }

        auto result = transfer.finish(std::nullopt, require_observed_validity);
        if (result || (result.error().code == ErrorCode::remote_gone &&
                          result.error().message == errors::remote_mail_gone)) {
            state_->selected_uid_validity = key.uid_validity;
        } else {
            state_->reset();
        }
        return result;
    };

    return retry_imap(fetch_once);
}

Result<imap_detail::CurlHandle> imap_detail::make_imap_handle(
    const RuntimeConfig& config, std::string_view mailbox_path) {
    return make_imap_handle_impl(config, mailbox_path);
}

// 对外独立接口内部使用临时会话完成有限重试。
Result<MailboxSnapshot> read_mailbox_snapshot(const RuntimeConfig& config) {
    return ImapSession{config}.read_mailbox_snapshot();
}

Result<FetchedMail> fetch_mail(const RuntimeConfig& config, const RemoteKey& key) {
    return ImapSession{config}.fetch_mail(key);
}

}  // namespace mailai

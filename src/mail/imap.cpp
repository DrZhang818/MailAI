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
#include "mailai/error_messages.h"

namespace mailai {
namespace {
namespace errors = error_messages::imap;

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

// 仅重试临时网络或服务故障。每次调用都重新创建句柄和响应缓冲区。
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

Result<imap_detail::CurlHandle> make_imap_handle_impl(const RuntimeConfig& config, std::string_view mailbox_path) {
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

    auto valid_text = [](std::string_view value) {
        return !value.empty() && value.find('\0') == std::string_view::npos;
    };

    // 1. 检查配置并读取密码
    if (!valid_text(config.account.id) || !valid_text(imap.host) || !valid_text(imap.username) ||
        !valid_text(imap.credential_env) || imap.credential_env.find('=') != std::string::npos) {
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

    const char* password_value = std::getenv(imap.credential_env.c_str());
    if (password_value == nullptr || password_value[0] == '\0') {
        return fail(errors::missing_password);
    }
    const std::string password{password_value};

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

    if (!set_option(CURLOPT_URL, server_url.get())) {
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
    if (!set_option(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(connect_ms)) ||
        !set_option(CURLOPT_TIMEOUT_MS, static_cast<long>(total_ms)) ||
        !set_option(CURLOPT_PROTOCOLS_STR, scheme) || !set_option(CURLOPT_NOSIGNAL, 1L) ||
        !set_option(CURLOPT_VERBOSE, 0L)) {
        return option_failure(errors::transfer_options_failed);
    }

    // 6. 返回已配置的 handle
    return handle;
}

// 收件箱快照：回调和单次请求。
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

Result<MailboxSnapshot> read_mailbox_snapshot_once(const RuntimeConfig& config) {
    auto fail = [&](ErrorCode code, const char* message, CURLcode status) {
        return std::unexpected(Error{
            .code = code,
            .message = std::string(message) + ": " + curl_easy_strerror(status),
            .account_id = config.account.id,
            .mail_id = std::nullopt,
            .stage = errors::snapshot_stage
        });
    };

    // 1. 让响应缓冲区的生命周期覆盖句柄和回调。
    std::string response;

    // 2. 创建 INBOX 连接句柄。
    auto handle = imap_detail::make_imap_handle(config, "/INBOX");
    if (!handle) {
        return std::unexpected(handle.error());
    }

    // 3. 设置查询命令和响应回调。
    CurlOptionSetter set_option{handle->get()};
    if (!set_option(CURLOPT_CUSTOMREQUEST, "UID SEARCH ALL") ||
        !set_option(CURLOPT_HEADERFUNCTION, &append_response) ||
        !set_option(CURLOPT_HEADERDATA, &response) ||
        !set_option(CURLOPT_WRITEFUNCTION, &discard_response)) {
        return fail(ErrorCode::config_error, errors::snapshot_options_failed, set_option.code);
    }

    // 4. 查询 INBOX 并检查传输结果。
    const CURLcode status = curl_easy_perform(handle->get());
    if (status != CURLE_OK) {
        if (imap_detail::has_service_unavailable(response)) {
            return fail(ErrorCode::network_error, errors::service_unavailable, status);
        }
        return fail(classify_transfer_error(status), errors::snapshot_request_failed, status);
    }

    // 5. 解析 UIDVALIDITY 和 UID 列表。
    return imap_detail::parse_mailbox_snapshot(response, config.account.id);
}

// 单封邮件下载：回调和单次请求。
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

Result<FetchedMail> fetch_mail_once(const RuntimeConfig& config, const RemoteKey& key) {
    auto make_error = [&](ErrorCode code, std::string message) {
        return Error{
            .code = code,
            .message = std::move(message),
            .account_id = config.account.id,
            .mail_id = std::nullopt,
            .stage = errors::fetch_stage
        };
    };

    // 1. 检查远端邮件标识，并指定邮箱的 UIDVALIDITY。
    if (key.account_id.empty() || key.account_id != config.account.id || key.folder != "INBOX" ||
        key.uid_validity == 0 || key.uid == 0) {
        return std::unexpected(make_error(ErrorCode::invalid_argument, errors::invalid_remote_key));
    }
    const auto path = "/INBOX;UIDVALIDITY=" + std::to_string(key.uid_validity);
    imap_detail::FetchTransfer transfer(key);
    auto handle = imap_detail::make_imap_handle(config, path);
    if (!handle) {
        return std::unexpected(handle.error());
    }

    // 2. 使用 BODY.PEEK[] 请求完整邮件，保持服务器已读状态不变。
    const auto command = "UID FETCH " + std::to_string(key.uid) + " (INTERNALDATE BODY.PEEK[])";
    CurlOptionSetter set_option{handle->get()};
    if (!set_option(CURLOPT_CUSTOMREQUEST, command.c_str()) ||
        !set_option(CURLOPT_HEADERFUNCTION, &fetch_header_callback) ||
        !set_option(CURLOPT_HEADERDATA, &transfer) ||
        !set_option(CURLOPT_WRITEFUNCTION, &fetch_body_callback) ||
        !set_option(CURLOPT_WRITEDATA, &transfer)) {
        return std::unexpected(make_error(ErrorCode::config_error,
            std::string(errors::fetch_options_failed) + ": " +
                curl_easy_strerror(set_option.code)));
    }

    // 3. 执行一次下载，将传输结果及已收集的数据交给协议解析组件。
    const auto status = curl_easy_perform(handle->get());
    if (status != CURLE_OK) {
        return transfer.finish(make_error(classify_transfer_error(status),
            std::string(errors::fetch_request_failed) + ": " + curl_easy_strerror(status)));
    }
    return transfer.finish();
}

}  // namespace

Result<imap_detail::CurlHandle> imap_detail::make_imap_handle(const RuntimeConfig& config, std::string_view mailbox_path) {
    return make_imap_handle_impl(config, mailbox_path);
}

// 对外接口负责有限重试。
Result<MailboxSnapshot> read_mailbox_snapshot(const RuntimeConfig& config) {
    return retry_imap([&] { return read_mailbox_snapshot_once(config); });
}

Result<FetchedMail> fetch_mail(const RuntimeConfig& config, const RemoteKey& key) {
    return retry_imap([&] { return fetch_mail_once(config, key); });
}

}  // namespace mailai

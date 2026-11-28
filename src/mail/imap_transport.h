#pragma once

#include <curl/curl.h>

#include <memory>
#include <string_view>

#include "mailai/config.h"

namespace mailai::imap_detail {

using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;

// 配置 IMAP 请求句柄，不立即发起传输。
Result<CurlHandle> make_imap_handle(
    const RuntimeConfig& config, std::string_view mailbox_path = "/");

}  // namespace mailai::imap_detail

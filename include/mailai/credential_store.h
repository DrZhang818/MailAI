#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "mailai/model.h"

namespace mailai {

struct CredentialStore {
    // 凭据不存在时返回空值，读取失败时返回错误。
    std::function<Result<std::optional<std::string>>(std::string_view)> read;
    std::function<Status(std::string_view, std::string_view)> write;
    std::function<Status(std::string_view)> erase;
};

CredentialStore make_keychain_store();

}  // namespace mailai

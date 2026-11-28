#pragma once

#include <functional>

#include "mailai/config.h"
#include "mailai/credential_store.h"
#include "mailai/sync_types.h"

namespace mailai::login_detail {

using VerifyLogin = std::function<Result<MailboxSnapshot>(const RuntimeConfig&)>;

Result<Environment> login_defaults(const Environment& env);
Result<std::filesystem::path> resolve_cache_directory(const Environment& env);
Result<RuntimeConfig> resolve_sync_settings(
    const Environment& env, const CredentialStore& credentials);
Status remember_login(const Environment& env,
    const Environment& input,
    std::string_view password,
    const CredentialStore& credentials,
    const VerifyLogin& verify);
Status forget_login(const Environment& env, const CredentialStore& credentials);

}  // namespace mailai::login_detail

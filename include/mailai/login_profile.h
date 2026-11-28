#pragma once

#include "mailai/config.h"

namespace mailai {

Result<std::filesystem::path> resolve_login_profile_dir(const Environment& env);

Status ensure_login_profile_dir(const Environment& env);

Result<std::optional<Environment>> load_login_profile(const Environment& env);

Status save_login_profile(const Environment& env, const Environment& profile);

}  // namespace mailai

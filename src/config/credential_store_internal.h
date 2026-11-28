#pragma once

#include <Security/Security.h>

#include "mailai/credential_store.h"

namespace mailai::detail {

struct KeychainCalls {
    std::function<OSStatus(CFDictionaryRef, CFTypeRef*)> copy_matching;
    std::function<OSStatus(CFDictionaryRef, CFTypeRef*)> add;
    std::function<OSStatus(CFDictionaryRef, CFDictionaryRef)> update;
    std::function<OSStatus(CFDictionaryRef)> erase;
};

// 将系统调用与查询构造分开，测试时无需访问真实钥匙串。
CredentialStore make_keychain_store(KeychainCalls calls);

}  // namespace mailai::detail

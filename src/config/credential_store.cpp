#include "mailai/credential_store.h"

#include "mailai/error_message.h"

#ifdef __APPLE__
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#include "credential_store_internal.h"
#endif

namespace mailai {

namespace {

namespace errors = error_message::keychain;

std::unexpected<Error> make_error(ErrorCode code, const char* message) {
    return std::unexpected(Error{code, message, std::nullopt, std::nullopt});
}

#ifdef __APPLE__

struct CfDeleter {
    void operator()(CFTypeRef value) const noexcept {
        CFRelease(value);
    }
};

template <class Ref>
using CfPtr = std::unique_ptr<std::remove_pointer_t<Ref>, CfDeleter>;

CfPtr<CFMutableDictionaryRef> make_dictionary() {
    return CfPtr<CFMutableDictionaryRef>{CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
}

bool valid_size(std::string_view value) {
    return value.size() <= static_cast<std::size_t>(std::numeric_limits<CFIndex>::max());
}

Result<CfPtr<CFMutableDictionaryRef>> make_query(std::string_view account_key) {
    if (account_key.empty() || account_key.find('\0') != std::string_view::npos ||
        !valid_size(account_key)) {
        return make_error(ErrorCode::invalid_argument, errors::invalid_account);
    }

    const CfPtr<CFStringRef> account{CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(account_key.data()),
        static_cast<CFIndex>(account_key.size()),
        kCFStringEncodingUTF8,
        false)};
    if (!account) {
        return make_error(ErrorCode::invalid_argument, errors::invalid_account);
    }

    auto query = make_dictionary();
    if (!query) {
        return make_error(ErrorCode::storage_error, errors::query_failed);
    }

    // 所有查询都限定为本程序服务下的一条指定账号凭据。
    CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.get(), kSecAttrService, CFSTR("com.mailai.assistant.imap"));
    CFDictionarySetValue(query.get(), kSecAttrAccount, account.get());
    return query;
}

Result<std::optional<std::string>> read_password(
    const detail::KeychainCalls& calls, std::string_view account_key) {
    const auto query = make_query(account_key);
    if (!query) {
        return std::unexpected(query.error());
    }

    CFDictionarySetValue(query->get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query->get(), kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const auto status = calls.copy_matching(query->get(), &result);
    const CfPtr<CFTypeRef> data{result};
    if (status == errSecItemNotFound) {
        return std::nullopt;
    }
    if (status != errSecSuccess) {
        return make_error(ErrorCode::auth_error, errors::read_failed);
    }
    if (!data || CFGetTypeID(data.get()) != CFDataGetTypeID()) {
        return make_error(ErrorCode::auth_error, errors::invalid_saved_password);
    }

    const auto password = static_cast<CFDataRef>(data.get());
    const auto length = CFDataGetLength(password);
    const auto bytes = CFDataGetBytePtr(password);
    if (length <= 0 || !bytes) {
        return make_error(ErrorCode::auth_error, errors::invalid_saved_password);
    }
    return std::optional<std::string>{
        std::in_place, reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length)
    };
}

Status write_password(
    const detail::KeychainCalls& calls, std::string_view account_key, std::string_view password) {
    const auto query = make_query(account_key);
    if (!query) {
        return std::unexpected(query.error());
    }
    if (password.empty() || !valid_size(password)) {
        return make_error(ErrorCode::invalid_argument, errors::invalid_password);
    }

    const CfPtr<CFDataRef> data{CFDataCreate(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(password.data()),
        static_cast<CFIndex>(password.size()))};
    const CfPtr<CFMutableDictionaryRef> attributes{CFDictionaryCreateMutableCopy(
        kCFAllocatorDefault, 0, query->get())};
    if (!data || !attributes) {
        return make_error(ErrorCode::storage_error, errors::write_failed);
    }

    CFDictionarySetValue(attributes.get(), kSecValueData, data.get());
    auto status = calls.add(attributes.get(), nullptr);
    if (status == errSecDuplicateItem) {
        // 账号凭据已存在时只替换密码字节，不改动其他属性。
        const auto updates = make_dictionary();
        if (!updates) {
            return make_error(ErrorCode::storage_error, errors::write_failed);
        }
        CFDictionarySetValue(updates.get(), kSecValueData, data.get());
        status = calls.update(query->get(), updates.get());
    }
    if (status != errSecSuccess) {
        return make_error(ErrorCode::storage_error, errors::write_failed);
    }
    return {};
}

Status erase_password(const detail::KeychainCalls& calls, std::string_view account_key) {
    const auto query = make_query(account_key);
    if (!query) {
        return std::unexpected(query.error());
    }
    const auto status = calls.erase(query->get());
    if (status != errSecSuccess && status != errSecItemNotFound) {
        return make_error(ErrorCode::storage_error, errors::erase_failed);
    }
    return {};
}

#endif

}  // namespace

#ifdef __APPLE__

CredentialStore detail::make_keychain_store(KeychainCalls calls) {
    return CredentialStore{
        [calls](std::string_view account_key) { return read_password(calls, account_key); },
        [calls](std::string_view account_key, std::string_view password) {
            return write_password(calls, account_key, password);
        },
        [calls = std::move(calls)](
            std::string_view account_key) { return erase_password(calls, account_key); }
    };
}

CredentialStore make_keychain_store() {
    return detail::make_keychain_store(
        {SecItemCopyMatching, SecItemAdd, SecItemUpdate, SecItemDelete});
}

#else

CredentialStore make_keychain_store() {
    const auto unsupported = make_error(ErrorCode::config_error, errors::unsupported);
    return CredentialStore{
        [unsupported](
            std::string_view) -> Result<std::optional<std::string>> { return unsupported; },
        [unsupported](std::string_view, std::string_view) -> Status { return unsupported; },
        [unsupported](std::string_view) -> Status { return unsupported; }
    };
}

#endif

}  // namespace mailai

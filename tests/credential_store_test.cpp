#include <array>
#include <map>

#include "config/credential_store_internal.h"
#include "test_support.h"

namespace {

using namespace mailai;

const std::string account_a = "fixture-account-a";
const std::string account_b = "fixture-account-邮箱";
const std::string password = std::string("fixture\0secret", 14) + '\xff';
const std::string replacement = "fixture-replacement";

std::string account_from(CFDictionaryRef query, CFIndex expected_count) {
    expect(CFDictionaryGetCount(query) == expected_count,
        "Keychain query should contain only the required attributes");
    const auto kind = CFDictionaryGetValue(query, kSecClass);
    expect(kind && CFEqual(kind, kSecClassGenericPassword),
        "Keychain query must select generic password items");
    const auto service = CFDictionaryGetValue(query, kSecAttrService);
    expect(service && CFEqual(service, CFSTR("com.mailai.assistant.imap")),
        "Keychain query must be scoped to MailAI's own service");
    const auto account = CFDictionaryGetValue(query, kSecAttrAccount);
    expect(account && CFGetTypeID(account) == CFStringGetTypeID(),
        "Keychain query must be scoped to a single account");
    std::array<char, 128> buffer{};
    expect(
        CFStringGetCString(
            static_cast<CFStringRef>(account), buffer.data(), buffer.size(), kCFStringEncodingUTF8),
        "Keychain account should preserve UTF-8 text");
    return buffer.data();
}

std::string password_from(CFDictionaryRef attributes) {
    const auto data = CFDictionaryGetValue(attributes, kSecValueData);
    expect(data && CFGetTypeID(data) == CFDataGetTypeID(),
        "Keychain write must pass the password as bytes");
    const auto value = static_cast<CFDataRef>(data);
    return {
        reinterpret_cast<const char*>(CFDataGetBytePtr(value)),
        static_cast<std::size_t>(CFDataGetLength(value))
    };
}

struct FakeKeychain {
    std::map<std::string, std::string> passwords;
    OSStatus read_status{errSecSuccess};
    OSStatus add_status{errSecSuccess};
    OSStatus update_status{errSecSuccess};
    OSStatus erase_status{errSecSuccess};
    bool malformed_read{};
    int calls{};

    CredentialStore store() {
        return detail::make_keychain_store(detail::KeychainCalls{
            [this](CFDictionaryRef query, CFTypeRef* result) -> OSStatus {
                ++calls;
                auto account = account_from(query, 5);
                expect(CFDictionaryGetValue(query, kSecReturnData) == kCFBooleanTrue,
                    "Keychain read must request password data");
                expect(CFDictionaryGetValue(query, kSecMatchLimit) == kSecMatchLimitOne,
                    "Keychain read must request at most one result");
                if (read_status != errSecSuccess) {
                    return read_status;
                }
                const auto it = passwords.find(account);
                if (it == passwords.end()) {
                    return errSecItemNotFound;
                }
                if (malformed_read) {
                    *result = CFRetain(CFSTR("unexpected data type"));
                    return errSecSuccess;
                }
                *result = CFDataCreate(kCFAllocatorDefault,
                    reinterpret_cast<const UInt8*>(it->second.data()),
                    it->second.size());
                return errSecSuccess;
            },
            [this](CFDictionaryRef attributes, CFTypeRef* result) -> OSStatus {
                ++calls;
                auto account = account_from(attributes, 4);
                expect(result == nullptr, "Keychain write must not request returned credentials");
                auto value = password_from(attributes);
                if (add_status != errSecSuccess) {
                    return add_status;
                }
                const auto inserted = passwords.emplace(account, value).second;
                return inserted ? errSecSuccess : errSecDuplicateItem;
            },
            [this](CFDictionaryRef query, CFDictionaryRef attributes) -> OSStatus {
                ++calls;
                auto account = account_from(query, 3);
                expect(CFDictionaryGetCount(attributes) == 1,
                    "Keychain replacement must update only the password data");
                auto value = password_from(attributes);
                if (update_status != errSecSuccess) {
                    return update_status;
                }
                if (!passwords.contains(account)) {
                    return errSecItemNotFound;
                }
                passwords.at(account) = value;
                return errSecSuccess;
            },
            [this](CFDictionaryRef query) -> OSStatus {
                ++calls;
                auto account = account_from(query, 3);
                if (erase_status != errSecSuccess) {
                    return erase_status;
                }
                return passwords.erase(account) ? errSecSuccess : errSecItemNotFound;
            }
        });
    }
};

void test_create_and_read_raw_password_bytes() {
    FakeKeychain fake;
    auto store = fake.store();
    expect(store.write(account_a, password).has_value(), "new Keychain password should be saved");
    const auto saved = store.read(account_a);
    expect(saved && *saved == password, "Keychain password should preserve every byte");
}

void test_replacement_and_erase_preserve_other_accounts() {
    FakeKeychain fake;
    auto store = fake.store();
    expect(store.write(account_a, password).has_value(), "first Keychain item should be saved");
    expect(store.write(account_b, password).has_value(), "second Keychain item should be saved");
    expect(store.write(account_a, replacement).has_value(), "existing Keychain item should update");
    auto saved = store.read(account_a);
    expect(saved && *saved == replacement, "replacement should be read from the first account");
    saved = store.read(account_b);
    expect(saved && *saved == password, "replacement should not change another account");
    expect(store.erase(account_a).has_value(), "erase should remove the selected account");
    expect(store.erase(account_a).has_value(), "erasing an absent account should succeed");
    saved = store.read(account_a);
    expect(
        saved && !saved->has_value(), "an erased password is absence rather than a read failure");
    saved = store.read(account_b);
    expect(saved && *saved == password, "erase should not remove another account");
}

void test_missing_and_invalid_saved_password() {
    FakeKeychain fake;
    auto store = fake.store();
    const auto missing = store.read(account_a);
    expect(missing && !missing->has_value(),
        "missing Keychain password is absence rather than a read failure");
    fake.passwords.emplace(account_a, "");
    const auto empty = store.read(account_a);
    expect(!empty && empty.error().code == ErrorCode::auth_error,
        "empty Keychain password must not be treated as usable");
    fake.malformed_read = true;
    const auto malformed = store.read(account_a);
    expect(!malformed && malformed.error().code == ErrorCode::auth_error,
        "non-data Keychain result must not be treated as a password");
}

void test_native_failures_are_reported_without_passwords() {
    FakeKeychain fake;
    auto store = fake.store();
    expect(store.write(account_a, password).has_value(),
        "a preexisting password should be saved before testing native failures");
    fake.read_status = errSecAuthFailed;
    const auto read_error = store.read(account_a);
    expect(!read_error && read_error.error().code == ErrorCode::auth_error,
        "Keychain read failure should be reported");
    fake.read_status = errSecSuccess;
    fake.add_status = errSecAuthFailed;
    const auto add_error = store.write(account_a, replacement);
    expect(!add_error && add_error.error().code == ErrorCode::storage_error,
        "Keychain add failure should be reported");
    expect(add_error.error().message.find(replacement) == std::string::npos,
        "Keychain add error must not include password bytes");
    auto saved = store.read(account_a);
    expect(saved && *saved == password,
        "a failed add must not be retried as an update unless the item exists error was returned");
    fake.add_status = errSecDuplicateItem;
    fake.update_status = errSecAuthFailed;
    const auto update_error = store.write(account_a, replacement);
    expect(!update_error && update_error.error().code == ErrorCode::storage_error,
        "Keychain update failure should be reported");
    expect(update_error.error().message.find(replacement) == std::string::npos,
        "Keychain update error must not include password bytes");
    fake.erase_status = errSecAuthFailed;
    const auto erase_error = store.erase(account_a);
    expect(!erase_error && erase_error.error().code == ErrorCode::storage_error,
        "Keychain erase failure should be reported");
    saved = store.read(account_a);
    expect(saved && *saved == password, "a failed update or erase must preserve the original item");
}

void test_invalid_inputs_do_not_invoke_keychain() {
    FakeKeychain fake;
    auto store = fake.store();
    const std::array<std::string, 3> invalid_accounts{"", std::string("a\0b", 3), "\xff"};
    for (const auto& account : invalid_accounts) {
        const auto read_result = store.read(account);
        const auto write_result = store.write(account, password);
        const auto erase_result = store.erase(account);
        expect(!read_result && read_result.error().code == ErrorCode::invalid_argument &&
                   !write_result && write_result.error().code == ErrorCode::invalid_argument &&
                   !erase_result && erase_result.error().code == ErrorCode::invalid_argument,
            "an invalid account key should be rejected by every Keychain operation");
    }
    const auto empty_password = store.write(account_a, "");
    expect(!empty_password && empty_password.error().code == ErrorCode::invalid_argument,
        "empty passwords should be rejected before touching Keychain");
    expect(fake.calls == 0, "invalid requests must never reach native Keychain functions");
}

}  // namespace

int main() {
    test_create_and_read_raw_password_bytes();
    test_replacement_and_erase_preserve_other_accounts();
    test_missing_and_invalid_saved_password();
    test_native_failures_are_reported_without_passwords();
    test_invalid_inputs_do_not_invoke_keychain();
}

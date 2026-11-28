#include "mailai/services.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "login_service.h"
#include "mailai/credential_store.h"
#include "mailai/data_dir_lock.h"
#include "mailai/error_message.h"
#include "mailai/imap.h"
#include "mailai/mime.h"
#include "mailai/store.h"
#include "mailai/sync.h"

namespace mailai {
namespace {

constexpr char cache_filename[] = "cache.sqlite3";

Error filesystem_error(std::string message, const std::error_code& error) {
    return {
        ErrorCode::storage_error,
        std::move(message) + ": " + error.message(),
        std::nullopt,
        std::nullopt,
        "storage.directory"
    };
}

Error cache_permission_error(const char* message) {
    return {ErrorCode::storage_error, message, std::nullopt, std::nullopt, "storage.directory"};
}

// 新建应用目录时直接使用私有权限；已有目录只检查，不修改其权限。
Status prepare_cache_directory(const std::filesystem::path& directory) {
    namespace errors = error_message::storage;
    auto target = directory;
    // 去掉末尾的空路径分量和当前目录分量，保留其他路径解析语义。
    while (target.filename().empty() || target.filename() == ".") {
        const auto parent = target.parent_path();
        if (parent.empty() || parent == target)
            break;
        target = parent;
    }
    std::error_code error;
    if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path(), error);
        if (error)
            return std::unexpected(filesystem_error(errors::create_directory_failed, error));
    }
    if (::mkdir(target.c_str(), 0700) != 0 && errno != EEXIST) {
        return std::unexpected(filesystem_error(
            errors::create_directory_failed, std::error_code{errno, std::generic_category()}));
    }

    struct stat info{};
    if (::stat(target.c_str(), &info) != 0) {
        return std::unexpected(filesystem_error(
            errors::inspect_cache_failed, std::error_code{errno, std::generic_category()}));
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0022) != 0) {
        return std::unexpected(cache_permission_error(errors::unsafe_cache_directory));
    }
    return {};
}

class CacheFileDescriptor {
public:
    explicit CacheFileDescriptor(int fd) noexcept : fd_(fd) {}
    ~CacheFileDescriptor() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    CacheFileDescriptor(const CacheFileDescriptor&) = delete;
    CacheFileDescriptor& operator=(const CacheFileDescriptor&) = delete;
    int get() const noexcept {
        return fd_;
    }

private:
    int fd_;
};

// 在 SQLite 打开文件前保护缓存；不跟随符号链接或更改其他文件。
Status protect_cache_file(const std::filesystem::path& path, bool create) {
    namespace errors = error_message::storage;
    const int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (create ? O_CREAT : 0);
    int fd;
    do {
        fd = ::open(path.c_str(), flags, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        if (!create && errno == ENOENT)
            return {};
        return std::unexpected(filesystem_error(
            errors::protect_cache_failed, std::error_code{errno, std::generic_category()}));
    }
    CacheFileDescriptor file(fd);

    struct stat info{};
    if (::fstat(file.get(), &info) != 0) {
        return std::unexpected(filesystem_error(
            errors::protect_cache_failed, std::error_code{errno, std::generic_category()}));
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1) {
        return std::unexpected(cache_permission_error(errors::unsafe_cache_file));
    }
    if ((info.st_mode & 07777) != 0600 && ::fchmod(file.get(), 0600) != 0) {
        return std::unexpected(filesystem_error(
            errors::protect_cache_failed, std::error_code{errno, std::generic_category()}));
    }
    return {};
}

Status protect_cache_files(const std::filesystem::path& directory) {
    auto main = protect_cache_file(directory / cache_filename, true);
    if (!main)
        return main;
    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
        auto protected_file =
            protect_cache_file(directory / (std::string(cache_filename) + suffix), false);
        if (!protected_file)
            return protected_file;
    }
    return {};
}

// 示例数据与真实缓存分开，查询示例不接触数据库。
const std::vector<Account>& demo_accounts() {
    static const std::vector<Account> accounts{
        {"school", "学校邮箱", "student@school.example"}, {"qq", "QQ邮箱", "student@qq.example"}
    };
    return accounts;
}

const std::vector<Mail>& demo_mails() {
    static const std::vector<Mail> mails{
        {44, "school", "library@school.example", "图书馆通知", "请按时归还图书", 100},
        {43, "qq", "recruiter@example.com", "实习通知", "请确认面试时间", 200},
        {42, "school", "course@school.example", "课程通知", "请查看课程安排", 200}
    };
    return mails;
}

Result<std::optional<SqliteStore>> open_read_cache(const Environment& env) {
    auto directory = login_detail::resolve_cache_directory(env);
    if (!directory)
        return std::unexpected(directory.error());

    const auto path = *directory / cache_filename;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return std::optional<SqliteStore>{};
    if (error)
        return std::unexpected(
            filesystem_error(error_message::storage::inspect_cache_failed, error));
    if (!std::filesystem::exists(status))
        return std::optional<SqliteStore>{};

    auto store = SqliteStore::open(path, OpenMode::read_only);
    if (!store)
        return std::unexpected(store.error());
    return std::optional<SqliteStore>{std::move(*store)};
}

Result<std::vector<InboxItem>> list_cache(const Environment& env, const InboxQuery& query) {
    auto cache = open_read_cache(env);
    if (!cache)
        return std::unexpected(cache.error());
    if (!*cache)
        return std::vector<InboxItem>{};
    return (*cache)->list(query);
}

Result<Mail> find_cache(const Environment& env, MailId id) {
    auto cache = open_read_cache(env);
    if (!cache)
        return std::unexpected(cache.error());
    if (!*cache) {
        return std::unexpected(Error{
            ErrorCode::mail_not_found,
            error_message::common::mail_not_found,
            std::nullopt,
            id,
            "storage.find"
        });
    }
    auto mail = (*cache)->find(id);
    if (!mail || mail->parse_state != ParseState::parsed)
        return mail;
    auto raw = (*cache)->read_raw(id);
    if (!raw)
        return std::unexpected(raw.error());
    // 从原文恢复表格，不修改已缓存的正文，也不需要重新连接服务器。
    if (auto blocks = parse_body_blocks(*raw); blocks)
        mail->body_blocks = std::move(*blocks);
    return mail;
}

Result<SyncSummary> sync_cache(
    const Environment& env, const CredentialStore& credentials, std::size_t history_limit) {
    auto config = login_detail::resolve_sync_settings(env, credentials);
    if (!config)
        return std::unexpected(config.error());

    auto directory = prepare_cache_directory(config->data_dir);
    if (!directory)
        return std::unexpected(directory.error());

    // 锁先于数据库取得，后于数据库释放，覆盖整个同步过程。
    auto lock = DataDirLock::acquire(config->data_dir);
    if (!lock)
        return std::unexpected(lock.error());

    auto protected_files = protect_cache_files(config->data_dir);
    if (!protected_files)
        return std::unexpected(protected_files.error());

    auto store = SqliteStore::open(config->data_dir / cache_filename, OpenMode::read_write_create);
    if (!store)
        return std::unexpected(store.error());

    return sync_inbox(*store, *config, history_limit);
}

}  // namespace

MailServices make_runtime_services(Environment env) {
    const auto credentials = make_keychain_store();
    return {
        [env](DataSource source, const InboxQuery& query) {
            if (source == DataSource::demo)
                return list_inbox(demo_accounts(), demo_mails(), query);
            return list_cache(env, query);
        },
        [env](DataSource source, MailId id) {
            if (source == DataSource::demo)
                return find_mail(demo_mails(), id);
            return find_cache(env, id);
        },
        [env, credentials](
            std::size_t history_limit) { return sync_cache(env, credentials, history_limit); },
        [env] { return login_detail::login_defaults(env); },
        [env, credentials](const Environment& input, std::string_view password) {
            return login_detail::remember_login(
                env, input, password, credentials, read_mailbox_snapshot);
        },
        [env = std::move(env), credentials] { return login_detail::forget_login(env, credentials); }
    };
}

}  // namespace mailai

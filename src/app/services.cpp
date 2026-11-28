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
#include "util/secure_fs.h"

namespace mailai {
namespace {

constexpr char cache_filename[] = "cache.sqlite3";
constexpr const char* cache_sidecar_suffixes[] = {"-wal", "-shm", "-journal"};

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

// 去掉末尾的空路径分量和当前目录分量，保留其他路径解析语义。
std::filesystem::path normalized_directory(const std::filesystem::path& directory) {
    auto target = directory;
    while (target.filename().empty() || target.filename() == ".") {
        const auto parent = target.parent_path();
        if (parent.empty() || parent == target)
            break;
        target = parent;
    }
    return target;
}

// 打开缓存目录并检查归属与写权限。缓存目录沿用原有语义，允许最后一级为指向本人目录的
// 符号链接；之后的文件操作都相对这个已检查的描述符进行。
Result<secure_fs::UniqueFd> open_cache_directory(const std::filesystem::path& directory) {
    namespace errors = error_message::storage;
    auto fd = secure_fs::open_directory(directory, secure_fs::FollowLink::yes);
    if (!fd)
        return std::unexpected(filesystem_error(errors::inspect_cache_failed, fd.error()));
    const auto info = secure_fs::stat_fd(fd->get());
    if (!info)
        return std::unexpected(filesystem_error(errors::inspect_cache_failed, info.error()));
    if (!S_ISDIR(info->st_mode) || !secure_fs::owned_by_current_user(*info) ||
        (info->st_mode & 0022) != 0) {
        return std::unexpected(cache_permission_error(errors::unsafe_cache_directory));
    }
    return std::move(*fd);
}

// 新建应用目录时使用私有权限，并保留已检查的目录描述符供后续操作使用。
Result<secure_fs::UniqueFd> prepare_cache_directory(const std::filesystem::path& directory) {
    namespace errors = error_message::storage;
    const auto target = normalized_directory(directory);
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

    return open_cache_directory(target);
}

Status verify_cache_directory_identity(const std::filesystem::path& path, int directory_fd) {
    namespace errors = error_message::storage;
    const auto resolved = secure_fs::open_directory(path, secure_fs::FollowLink::no);
    if (!resolved)
        return std::unexpected(filesystem_error(errors::inspect_cache_failed, resolved.error()));
    const auto checked = secure_fs::stat_fd(directory_fd);
    if (!checked)
        return std::unexpected(filesystem_error(errors::inspect_cache_failed, checked.error()));
    const auto current = secure_fs::stat_fd(resolved->get());
    if (!current)
        return std::unexpected(filesystem_error(errors::inspect_cache_failed, current.error()));
    if (checked->st_dev != current->st_dev || checked->st_ino != current->st_ino)
        return std::unexpected(cache_permission_error(errors::unsafe_cache_directory));
    return {};
}

Result<std::filesystem::path> resolve_checked_cache_path(
    const std::filesystem::path& directory, int directory_fd) {
    std::error_code error;
    auto resolved = std::filesystem::canonical(directory, error);
    if (error) {
        return std::unexpected(
            filesystem_error(error_message::storage::inspect_cache_failed, error));
    }
    if (auto same = verify_cache_directory_identity(resolved, directory_fd); !same)
        return std::unexpected(same.error());
    // 保留合法的目录别名，只将已检查的目录路径交给 SQLite，数据库文件名原样拼接。
    return resolved / cache_filename;
}

// 在 SQLite 打开文件前保护缓存；不跟随符号链接或更改其他文件。
Status protect_cache_file(int directory_fd, const std::string& name, bool create) {
    namespace errors = error_message::storage;
    const int flags = O_RDONLY | O_NONBLOCK | (create ? O_CREAT : 0);
    auto file = secure_fs::open_at(directory_fd, name, flags, 0600);
    if (!file) {
        if (!create && file.error() == std::errc::no_such_file_or_directory)
            return {};
        return std::unexpected(filesystem_error(errors::protect_cache_failed, file.error()));
    }

    const auto info = secure_fs::stat_fd(file->get());
    if (!info)
        return std::unexpected(filesystem_error(errors::protect_cache_failed, info.error()));
    if (!S_ISREG(info->st_mode) || !secure_fs::owned_by_current_user(*info) ||
        info->st_nlink != 1) {
        return std::unexpected(cache_permission_error(errors::unsafe_cache_file));
    }
    if ((info->st_mode & 07777) != 0600 && ::fchmod(file->get(), 0600) != 0) {
        return std::unexpected(filesystem_error(
            errors::protect_cache_failed, std::error_code{errno, std::generic_category()}));
    }
    return {};
}

Status protect_cache_files(int directory_fd) {
    auto main = protect_cache_file(directory_fd, cache_filename, true);
    if (!main)
        return main;
    for (const auto* suffix : cache_sidecar_suffixes) {
        auto protected_file =
            protect_cache_file(directory_fd, std::string(cache_filename) + suffix, false);
        if (!protected_file)
            return protected_file;
    }
    return {};
}

Result<bool> inspect_cache_entry(int directory_fd, const char* name) {
    struct stat info{};
    if (::fstatat(directory_fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        const std::error_code error{errno, std::generic_category()};
        if (error == std::errc::no_such_file_or_directory)
            return false;
        return std::unexpected(
            filesystem_error(error_message::storage::inspect_cache_failed, error));
    }
    if (!S_ISREG(info.st_mode))
        return std::unexpected(cache_permission_error(error_message::storage::unsafe_cache_file));
    return true;
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

    namespace errors = error_message::storage;
    const auto target = normalized_directory(*directory);
    const auto directory_fd = secure_fs::open_directory(target, secure_fs::FollowLink::yes);
    if (!directory_fd) {
        if (directory_fd.error() == std::errc::no_such_file_or_directory)
            return std::optional<SqliteStore>{};
        return std::unexpected(
            filesystem_error(errors::inspect_cache_failed, directory_fd.error()));
    }
    const auto database_exists = inspect_cache_entry(directory_fd->get(), cache_filename);
    if (!database_exists)
        return std::unexpected(database_exists.error());
    if (!*database_exists)
        return std::optional<SqliteStore>{};
    for (const auto* suffix : cache_sidecar_suffixes) {
        const auto name = std::string(cache_filename) + suffix;
        if (auto inspected = inspect_cache_entry(directory_fd->get(), name.c_str()); !inspected)
            return std::unexpected(inspected.error());
    }

    auto path = resolve_checked_cache_path(target, directory_fd->get());
    if (!path)
        return std::unexpected(path.error());
    auto store = SqliteStore::open(*path, OpenMode::read_only);
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

    const auto directory_fd = prepare_cache_directory(config->data_dir);
    if (!directory_fd)
        return std::unexpected(directory_fd.error());
    const auto path =
        resolve_checked_cache_path(normalized_directory(config->data_dir), directory_fd->get());
    if (!path)
        return std::unexpected(path.error());

    // 锁先于数据库取得，后于数据库释放，覆盖整个同步过程。
    auto lock = DataDirLock::acquire(path->parent_path());
    if (!lock)
        return std::unexpected(lock.error());

    auto protected_files = protect_cache_files(directory_fd->get());
    if (!protected_files)
        return std::unexpected(protected_files.error());

    if (auto same = verify_cache_directory_identity(path->parent_path(), directory_fd->get());
        !same)
        return std::unexpected(same.error());
    auto store = SqliteStore::open(*path, OpenMode::read_write_create);
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

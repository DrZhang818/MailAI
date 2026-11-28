# Dependencies

当前开发平台为 macOS。完整选型见[阶段 2 计划](superpowers/plans/phase-02-mail-sync/plan.md)。经用户确认，libcurl 最低版本由 8.0 提高至 8.17.0，采用该版本修复后的自定义 FETCH 字面量处理能力；其他依赖版本下限不变。此调整记录于[阶段进度](superpowers/plans/phase-02-mail-sync/progress.md)。

## macOS 本机验证

macOS 27.2.1（Apple Silicon）、Apple Clang 21.0.0、CMake 4.4.5、Ninja 1.13.2、pkg-config 3.0.10；CMake 选用 Homebrew SQLite 3.54.1、libcurl 8.23.0、GMime 3.2.15、libxml2 2.15.4 和 libffi 3.8.0。Homebrew curl 的版本信息列出 IMAP、IMAPS 和 SSL；此检查不连接服务器。Python 3.14.10 和 OpenSSL 命令行 4.1.0 已安装。

本轮在受管理临时目录中按 README 的依赖选择方式独立构建 Debug 和 Release，两种模式各 11 项 CTest 全部通过，构建无警告。`store` 验证数据库生命周期、约束与失败恢复。`store_list`、`store_find` 使用真实临时 SQLite 验证列表筛选与排序、详情与附件、ID 边界、只读访问、数据保留和查询失败恢复。`store_sync` 验证同步状态与待办查询、任务登记、UIDVALIDITY 变化及事务回滚。`store_write` 验证邮件与附件保存、稳定 ID、解析失败后重试、远端消失后停止重试、同步结果记录、重开持久性及写入失败回滚。`sqlite_context` 验证共享 SQL 操作的参数复制、空值类型、语句复用、执行错误和事务失败恢复。`data_dir_lock` 验证跨进程争锁、默认与自定义超时、等待后取得、进程退出和移动后的释放、数据保留及持锁期间的只读缓存访问。写命令入口的取得锁调用由后续 CLI 接入任务实现。

Task 7 本机回归：Debug 与独立 Release 配置选用上述库，构建均无警告，默认 CTest 各 21/21 通过。另显式构建 `imap_live_probe`，其独立虚构 TLS 邮箱测试在两种构建下通过，验证启用门槛、最多五封、解析统计、不修改已读状态、不创建缓存及诊断脱敏。真实邮箱的验收结果单独记录在阶段进度中；该回归没有访问真实邮箱。

macOS 登录记忆功能使用系统自带的 Security.framework 和 CoreFoundation.framework，无需安装额外 Homebrew 依赖。Debug／Release 构建均无警告，默认 CTest 各 24/24 通过。钥匙串组件测试模拟系统调用，真实伪终端与虚构 TLS 邮箱验证密码隐藏和认证失败处理，未访问真实钥匙串或真实邮箱。Linux 登录功能留待后续设计。

使用本机系统版本作为部署目标，程序未验证对较旧 macOS 的兼容性。当前完整阶段代码尚未在 Linux 执行。

## 此前 WSL Ubuntu 验证

下表与后文保留此前 WSL Ubuntu 的实测记录，不能视为 macOS 的版本数据。

| 依赖 | 用途 | 开发包 | 最低版本 | 实测版本 | CMake 目标 | 接入状态 |
|---|---|---|---|---|---|---|
| SQLite | 本地邮件与同步状态存储 | `libsqlite3-dev` | 3.37 | 3.46.1 | `SQLite3::SQLite3` | 已接入 |
| libcurl | 邮箱服务器通信 | `libcurl4-openssl-dev` | 8.17.0 | 8.18.0 | `CURL::libcurl` | 已接入，协议能力已检查 |
| GMime | 邮件原文解析 | `libgmime-3.0-dev` | 3.2 | 3.2.15 | `PkgConfig::GMIME` | 已接入 |
| libxml2 | HTML 正文解析 | `libxml2-dev` | 2.9 | 2.15.2 | `LibXml2::LibXml2` | 已接入 |

SQLite 由 `find_package(SQLite3 3.37 REQUIRED)` 查找，作为 `mailai_core` 的 PRIVATE 依赖。已通过 WSL 的 CMake 配置、Debug 构建和现有 4 项 CTest，构建无警告；真实数据库读写与恢复行为在阶段 2 Task 2 验证。

libcurl 目前由 `find_package(CURL 8.17.0 REQUIRED)` 查找，同样作为 `mailai_core` 的 PRIVATE 依赖。此前使用 libcurl 8.18.0 通过 WSL CMake 配置、构建与当时的 4 项 CTest，构建无警告。

libcurl 能力通过临时 C++ 程序调用 `curl_version_info` 检查，使用项目 CMake 缓存选中的头文件目录 `/usr/include/x86_64-linux-gnu` 和库 `/usr/lib/x86_64-linux-gnu/libcurl.so`。探针编译、运行均成功：头文件与运行库均为 8.18.0，IMAP／IMAPS／TLS 均启用，TLS 后端为 OpenSSL/3.5.5。此检查不连接网络；实际 IMAP 收信、证书验证和服务器兼容性由 Task 4、7 验证。

GMime 通过 `find_package(PkgConfig REQUIRED)` 和 `pkg_check_modules(GMIME REQUIRED IMPORTED_TARGET "gmime-3.0>=3.2")` 查找，作为 `mailai_core` 的 PRIVATE 依赖。WSL pkg-config 和 CMake 均找到 3.2.15，配置、Debug 构建与现有 4 项 CTest 通过，无编译警告；邮件解析样本在 Task 3 验证。

libxml2 由 `find_package(LibXml2 2.9 REQUIRED)` 查找，作为 `mailai_core` 的 PRIVATE 依赖。WSL pkg-config 和 CMake 均确认版本为 2.15.2，配置、Debug 构建与现有 4 项 CTest 通过，无编译警告；HTML 处理样本在 Task 3 验证。

测试辅助工具已在 WSL 核对：Python 3.14.4、OpenSSL 命令行及运行库 3.5.5、pkg-config 2.5.1。Python 与 OpenSSL 命令行供后续本机 TLS 测试服务器使用，相关测试尚未实现。

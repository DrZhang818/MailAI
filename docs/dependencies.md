# Dependencies

依赖在 WSL Ubuntu 中使用系统开发包获取。完整选型和版本下限见[阶段 2 计划](superpowers/plans/mail-ai-assistant-stage-2.md)；此文件记录实际接入结果。

| 依赖 | 用途 | 开发包 | 最低版本 | 实测版本 | CMake 目标 | 接入状态 |
|---|---|---|---|---|---|---|
| SQLite | 本地邮件与同步状态存储 | `libsqlite3-dev` | 3.37 | 3.46.1 | `SQLite::SQLite3` | 已接入 |
| libcurl | 邮箱服务器通信 | `libcurl4-openssl-dev` | 8.0 | 8.18.0 | `CURL::libcurl` | 已接入，协议能力已检查 |
| GMime | 邮件原文解析 | `libgmime-3.0-dev` | 3.2 | 3.2.15 | `PkgConfig::GMIME` | 已接入 |
| libxml2 | HTML 正文解析 | `libxml2-dev` | 2.9 | 2.15.2 | `LibXml2::LibXml2` | 已接入 |

SQLite 由 `find_package(SQLite3 3.37 REQUIRED)` 查找，作为 `mailai_core` 的 PRIVATE 依赖。已通过 WSL 的 CMake 配置、Debug 构建和现有 4 项 CTest，构建无警告；真实数据库读写与恢复行为在阶段 2 Task 2 验证。

libcurl 由 `find_package(CURL 8.0 REQUIRED)` 查找，同样作为 `mailai_core` 的 PRIVATE 依赖。已通过 WSL CMake 配置、构建与现有 4 项 CTest，构建无警告。

libcurl 能力通过临时 C++ 程序调用 `curl_version_info` 检查，使用项目 CMake 缓存选中的头文件目录 `/usr/include/x86_64-linux-gnu` 和库 `/usr/lib/x86_64-linux-gnu/libcurl.so`。探针编译、运行均成功：头文件与运行库均为 8.18.0，IMAP／IMAPS／TLS 均启用，TLS 后端为 OpenSSL/3.5.5。此检查不连接网络；实际 IMAP 收信、证书验证和服务器兼容性由 Task 4、7 验证。

GMime 通过 `find_package(PkgConfig REQUIRED)` 和 `pkg_check_modules(GMIME REQUIRED IMPORTED_TARGET "gmime-3.0>=3.2")` 查找，作为 `mailai_core` 的 PRIVATE 依赖。WSL pkg-config 和 CMake 均找到 3.2.15，配置、Debug 构建与现有 4 项 CTest 通过，无编译警告；邮件解析样本在 Task 3 验证。

libxml2 由 `find_package(LibXml2 2.9 REQUIRED)` 查找，作为 `mailai_core` 的 PRIVATE 依赖。WSL pkg-config 和 CMake 均确认版本为 2.15.2，配置、Debug 构建与现有 4 项 CTest 通过，无编译警告；HTML 处理样本在 Task 3 验证。

测试辅助工具已在 WSL 核对：Python 3.14.4、OpenSSL 命令行及运行库 3.5.5、pkg-config 2.5.1。Python 与 OpenSSL 命令行供后续本机 TLS 测试服务器使用，相关测试尚未实现。

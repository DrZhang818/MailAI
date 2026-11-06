# SQLite 同步状态读取实施计划

> **For agentic workers:** 业务源码由用户按参考代码跟写，导师维护测试、构建接入与验证。沿用已批准的阶段 2 设计。

**Goal:** 按账号和文件夹读取持久同步状态，为后续原子登记待办提供查询入口。

**Architecture:** `SqliteStore::state` 通过一条 `accounts LEFT JOIN sync_state` 查询区分未知账号和尚无同步记录的已知账号。单条查询提供一致的读取，不额外开启显式事务，允许后续 `schedule` 在自己的写事务中调用。同步持久化代码放在 `src/store/sqlite_sync.cpp`，连接生命周期和邮件查询保留在原文件。

**Scope:** 本计划只交付 `state`；`schedule`、`pending`、邮件写入与网络接入留待后续步骤。

**Tech Stack:** C++23、SQLite 3.37+、CMake 3.20+、Ninja、CTest、macOS。

**Spec:** [阶段 2 Task 2 与接口契约](mail-ai-assistant-stage-2.md#task-2-sqlite-缓存与可靠写入)、[版本 1 数据模型](../specs/mail-ai-assistant-sqlite-lifecycle-design.md)。

**Status:** 用户按参考代码完成带写，导师按委托补齐 CMake 并修正列名拼写与首次读取状态判断。macOS Debug／Release 各 8 项 CTest 全部通过，构建无警告，格式与差异检查通过，独立代码审阅未发现需修改的缺陷；本步只完成同步状态读取，Task 2 仍在进行。

## Global Constraints

- 业务代码由用户按参考代码跟写；导师维护测试与 CMake，并修复用户明确委托的问题；提交和推送须有用户明确要求。
- 不修改版本 1 DDL、不创建同步记录、不修改任何已有业务数据；只读和可写连接均支持查询。
- 账号或文件夹为空时返回 `invalid_argument`；非空但未知的账号返回 `unknown_account`。
- 已知账号没有对应文件夹的同步记录时返回 `SyncState{}`，不将其当成未知账号。
- 错误包含输入 `account_id`，`mail_id` 为空，阶段为 `storage.state`；SQLite 错误沿用 `sqlite_error` 的代码映射。
- `uid_validity` 为 NULL 或 INTEGER 1–4294967295；`last_discovered_uid` 为 INTEGER 0–4294967295。没有 UIDVALIDITY 时发现进度必须是 0。
- 时间戳允许 NULL 或完整的有符号 64 位 INTEGER；0 和负数是有效值。类型或范围不符合上述契约时返回 `storage_error`，禁止静默截断。
- 参数通过长度明确的绑定传入；结果拥有自己的值，语句完成后连接可继续使用。

## Review Focus

1. 未知账号不能被误报为尚未同步的已知账号。
2. 文件夹不存在时返回空状态，不能混入同账号另一文件夹或另一账号的状态。
3. NULL、0、负时间戳和 UINT32_MAX 不能因真假判断或整数转换丢失含义。
4. SQLite 宽松类型及被绕过的 CHECK 约束不能导致无声截断或回绕。
5. 只读查询与错误路径不得创建记录或妨碍后续调用；参数中的 SQL 片段按普通文本处理。

## Task 1: 读取同步状态

**Files:** 用户修改 `include/mailai/store.h`，创建 `src/store/sqlite_sync.cpp`；导师维护 `tests/store_sync_test.cpp`、`CMakeLists.txt` 和本计划。

**Interface:** `Result<SyncState> SqliteStore::state(const AccountId& account_id, std::string_view folder)`。公开头文件显式包含 `<string_view>` 和 `mailai/sync_types.h`；消费已有 `SyncState`、`prepare`、`sqlite_error`，不增加公开辅助接口。

**Steps:**

- [x] 准备真实临时数据库测试，并验证功能缺失时测试明确失败。
- [x] 添加公开声明和实现文件，在 `mailai_core` 源文件列表中接入新文件。
- [x] 完成输入校验、参数绑定、LEFT JOIN 查询、字段校验和语句收尾。
- [x] 导师验证已知空状态、未知账号、过滤、NULL／整数边界、错误恢复和数据保留，并移除测试中暂用于报告缺失接口的编译期分支。
- [x] 构建并运行 Debug／Release 全套 CTest，检查格式和差异，按真实结果更新状态。

**Test scenarios:**

- 已有 `school` 账号但没有同步记录：两种打开模式均返回空状态，查询不插入记录；未知账号返回 `unknown_account`，空参数返回 `invalid_argument`。
- `school/INBOX` 的 UIDVALIDITY=100、发现进度=42、尝试时间=0、成功时间=-10 精确返回；不同文件夹及不同账号相互隔离，带 SQL 片段的文件夹仅作为值。
- UIDVALIDITY／发现进度达到 4294967295，时间戳达到 INT64_MIN／INT64_MAX 时完整返回；数据库重开后结果保持，关闭连接后返回值仍有效。
- 绕过 CHECK 后插入的越界值，以及 SQLite 可接受的 REAL／TEXT 字段，均返回 `storage_error`；修复测试夹具后同一连接可再次读取。
- 打开连接后暂时移除 `sync_state` 表：查询明确失败；按原 DDL 恢复后同一连接可重试。正常查询前后目录和业务数据保持不变。

**Verification:** `cmake --build build/macos-debug --target store_sync_test`；`ctest --test-dir build/macos-debug -R '^store_sync$' --output-on-failure`，预期通过。

最终运行 `cmake --build build/macos-debug` 和 `ctest --test-dir build/macos-debug --output-on-failure`，预期含 `store_sync` 的 8 项 CTest 全部通过且无构建警告。对本任务 C++ 文件运行 `clang-format --dry-run --Werror`，并运行 `git diff --check`，预期退出 0。

推送前运行 `cmake --build build/verify-sqlite-release` 和 `ctest --test-dir build/verify-sqlite-release --output-on-failure`，预期 8/8 通过且无构建警告。

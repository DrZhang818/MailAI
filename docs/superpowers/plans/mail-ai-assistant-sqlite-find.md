# SQLite 邮件详情查询实施计划

**Goal:** 按本地邮件 ID 读取完整 `Mail` 和有序附件信息。

**Architecture:** `SqliteStore::find` 先检查 ID，再在同一短读事务内分别查询 `messages` 和 `attachments`，将结果复制到拥有字符串和容器的 `Mail`。沿用现有 SQLite helpers 和 RAII，不修改表结构或其他查询。

**Scope:** 业务代码由用户按参考代码跟写，导师维护验收测试；不实现写入或接入 CLI。

**Tech Stack:** C++23、SQLite 3.37+、CMake 3.20+、Ninja、CTest、macOS。

**Spec:** [阶段 2 接口与 Task 2](mail-ai-assistant-stage-2.md#task-2-sqlite-缓存与可靠写入)、[版本 1 数据模型](../specs/mail-ai-assistant-sqlite-lifecycle-design.md)。

**Status:** 用户按参考代码完成带写并通过实际验收：macOS Debug／Release 各 7 项 CTest 全部通过，构建无警告，本任务文件的格式与差异检查通过，独立代码审阅未发现需修改的缺陷。本步只完成详情读取，Task 2 的写入和同步接口仍待实现。

## Global Constraints

- 保留已有修改；不直接代写业务源码，不自动提交或推送。
- 不修改 DDL；查询不修改已有业务数据；只读模式可用。
- `find(0)` 返回 `invalid_argument`；`id > INT64_MAX` 在绑定前返回 `mail_not_found`；范围内但不存在也返回 `mail_not_found`。这些错误的 `mail_id` 为输入 ID，阶段为 `storage.find`。
- SQL 执行错误沿用现有映射，阶段为 `storage.find`；事务错误保留 `storage.transaction`；中途失败不返回部分邮件。
- 无附件仍成功；附件按 `ordinal ASC` 返回。返回数据不依赖语句或连接的生命周期。
- UIDVALIDITY／UID 转为 `uint32_t` 前检查 1–4294967295；附件大小转为 `uint64_t` 前检查非负；未知解析状态返回 `storage_error`。

## Review Focus

1. `INT64_MAX` 可以查到，`INT64_MAX+1` 和 `UINT64_MAX` 不能因截断匹配负数 ID。
2. 邮件不存在与存在但无附件必须区分。
3. 邮件正文和附件名称等字符串需要保留完整长度及中文；关闭连接后仍然有效。
4. 附件按序号排列且不混入其他邮件的附件，序号不要求连续。
5. 第二次查询或行转换失败时返回错误并结束事务，后续调用可恢复。

## Task 1: 查询邮件详情

**Files:** 用户修改 `include/mailai/store.h`、`src/store/sqlite_store.cpp`；导师维护 `tests/store_find_test.cpp`、`CMakeLists.txt` 和本计划。

**Interface:** `Result<Mail> SqliteStore::find(MailId id)`；消费现有 `Mail`、`AttachmentInfo`、`prepare`、`sqlite_error`、`Transaction`，不增加公开辅助接口。`Mail` 没有 raw 字段，详情查询不返回原始邮件 BLOB。

**Steps:**

- [x] 导师准备真实临时数据库测试，并对占位及参考实现验证断言。
- [x] 用户按参考代码补充公开声明、ID 范围检查、主记录读取、附件读取和读事务收尾。
- [x] 导师接入 CMake 的 `store_find_test`／CTest `store_find`，验证错误上下文、字段映射、边界、附件顺序、数据保留和失败恢复。
- [x] 构建并运行 Debug 全套 CTest，检查本任务文件格式和差异；按实际结果更新验收状态。

**Verification:** `cmake --build build/macos-debug`；`ctest --test-dir build/macos-debug --output-on-failure`；最终预期含 `store_find` 的 7 项 CTest 通过且无编译警告。`clang-format --dry-run --Werror include/mailai/store.h src/store/sqlite_store.cpp tests/store_find_test.cpp` 和 `git diff --check` 预期退出 0。

推送前同时运行 `cmake --build build/verify-sqlite-release` 和 `ctest --test-dir build/verify-sqlite-release --output-on-failure`，预期 7/7 通过且无构建警告。

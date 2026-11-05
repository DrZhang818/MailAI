# SQLite 邮件列表查询实施计划

**Goal:** 在现有版本 1 数据库上实现只读邮件列表查询，支持账号筛选、账号名称关联和固定排序。

**Architecture:** `SqliteStore::list` 使用 SQLite JOIN 将 `messages` 与 `accounts` 关联，按需绑定账号参数并复制结果到 `InboxItem`。指定账号时先验证存在性，相关读取处于同一短读事务。沿用内部语句与事务 RAII。

**Scope:** 本步只交付列表查询；详情查询是下一步。业务源码由用户跟写，导师维护测试；不更改 DDL 或接入 CLI。

**Tech Stack:** C++23、SQLite 3.37+、CMake 3.20+、Ninja、CTest、macOS。

**Spec:** [阶段 2 接口契约和 Task 2](mail-ai-assistant-stage-2.md#task-2-sqlite-缓存与可靠写入)、[整体设计](../specs/mail-ai-assistant-design.md)、[版本 1 结构](../specs/mail-ai-assistant-sqlite-lifecycle-design.md)。用户已要求按讨论后的 JOIN 方案带写。

**Status:** 已按参考代码完成带写并验收：macOS Debug／Release 各 7 项 CTest 通过，构建无警告，本任务文件的格式及差异检查通过，独立代码审阅未发现需修改的缺陷。本步只完成列表查询，Task 2 的其余接口按阶段计划推进。

## Global Constraints

- 保留已有数据及工作区修改，不直接代写业务源码，不自动提交或推送。
- 查询不创建账号、不写邮件、不修改数据库结构；只读连接可用。
- 不把用户参数拼接进 SQL；使用绑定参数。返回对象拥有自己的字符串。
- 查询的 SQLite 错误沿用现有映射，阶段为 `storage.list`；事务辅助错误保留 `storage.transaction`。指定账号不存在时返回 `unknown_account` 并填入 `Error.account_id`。
- 发现非正数本地 ID 或未知解析状态时返回 `storage_error`，不返回部分列表；不得把负数 ID 转成无符号数。

## Review Focus

1. 空数据库与已知空账号返回空列表；未知账号返回错误，不能因 JOIN 结果为空而混淆。
2. 相同时间以 ID 升序排序，旧时间的小 ID 不得排到新邮件之前。
3. 账号 ID 含引号时仍能正确筛选；不发生 SQL 注入。
4. 解析失败和历史邮件仍可列出，账号名称和状态正确。
5. 关闭数据库后返回值仍然有效；只读连接查询不修改结构或记录。

## Task 1: 读取邮件列表

**Files:** 用户修改 `include/mailai/store.h`、`src/store/sqlite_store.cpp`；导师维护 `tests/store_list_test.cpp` 和 `CMakeLists.txt`。

**Interfaces:** 使用 `InboxQuery` 和 `InboxItem`，新增 `Result<std::vector<InboxItem>> SqliteStore::list(const InboxQuery&)`。`query.account_id` 为空表示全部账号；有值时验证账号存在并仅返回其邮件，空字符串不代表全部账号。

**Output:** 每行按 `id, account_id, account_name, sender, subject, received_at_utc, parse_state, historical` 映射；按 `received_at_utc DESC, id ASC` 排序。没有记录时返回成功的空 vector。

**Steps:**

- [x] 用户按参考代码补充接口声明及占位函数。
- [x] 导师接入真实临时数据库测试，覆盖上述五项关注点及错误传播；独立预期排序为 `42,43,44,7`，其中 `id=7/time=50`。
- [x] 运行测试，确认当前业务实现缺失。
- [x] 用户按参考代码跟写账号检查、参数绑定、结果复制和事务结束；导师逐段讲解并检查。
- [x] 验证指定账号、全部账号、空结果、错误与数据保留行为，运行 Debug 全套 CTest 和格式检查。

**Verification:** `cmake --build build/macos-debug` 和 `ctest --test-dir build/macos-debug --output-on-failure`；最终预期全部通过且无编译警告。`clang-format --dry-run --Werror include/mailai/store.h src/store/sqlite_store.cpp tests/store_list_test.cpp` 和 `git diff --check` 预期退出 0。

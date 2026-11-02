# SQLite 数据库生命周期实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 替换账号库的零散检查，交付完整版本 1 的打开、初始化、迁移、结构校验和失败恢复。

**Architecture:** `SqliteStore::open` 协调连接配置、`ensure_schema` 和 WAL；固定 DDL 与目录比较集中在 schema 模块；SQLite 内部辅助管理语句、错误和事务。测试使用独立旧库夹具及共享连接辅助。

**Tech Stack:** C++23、SQLite 3.37+、CMake 3.20+、Ninja、CTest、macOS。

**Spec:** [已批准的生命周期设计](../specs/mail-ai-assistant-sqlite-lifecycle-design.md)，隶属[阶段 2](mail-ai-assistant-stage-2.md) Task 2。

**Status:** 数据库生命周期已完成。独立代码审阅未发现缺陷；Debug／Release 各 5 项 CTest 全部通过，构建无警告。其余 Task 2 业务接口仍未实现。

## Global Constraints

- 保留工作区中的既有修改，不重置、不自动提交；在当前共享工作区完成用户指定修改。
- 公共 `SqliteStore::open(path, OpenMode)` 和 `ensure_account` 的业务契约保持；新增类型均位于内部 `mailai::store_detail`。
- 五表及两索引组成版本 1；版本 0 只识别空库和提交 `c0c537b` 的已知账号库；不支持版本返回 `storage_error`。
- SQLite 忙错误映射为 `busy`；连接等待 1000 毫秒。初始化／迁移中版本更新与 DDL 同一事务。
- WAL 在 schema 提交后独立配置并验证；失败允许留下完整有效的版本 1，不能声称回滚版本。
- 只读连接不初始化、迁移、改版本或切换 WAL。不得暴露测试专用公共 API 或输出凭据／记录内容。
- 只实现生命周期，不提前实现邮件业务读写、同步编排或进程锁。

## Review Focus

1. 旧账号库的 SQL 空白与历史定义必须匹配：独立冻结旧库夹具；迁移前后比较原账号记录和定义，Task 1。
2. 增加触发器、CHECK 或 UNIQUE 不能通过列形状检查：目录变体测试覆盖额外对象和约束，Task 1。
3. 中途 DDL、版本写入和 COMMIT 失败不能留下半套 schema：测试连接 authorizer 控制失败，Task 2。
4. WAL 失败发生在 schema 已提交之后：验证版本 1 保留且可重试，Task 2。
5. 统计表、VACUUM 和内存数据库不能被误判为未知格式：实际 SQLite 测试这些操作后的打开行为，Task 2。

## Task 1: 固定结构、版本与事务迁移

**Files:** 新建 `src/store/schema.h/.cpp`、`src/store/sqlite_utils.h/.cpp`、`tests/store_test_support.h`、`tests/fixtures/store/legacy_accounts.sql`；修改 `src/store/sqlite_store.cpp`、`tests/store_test.cpp`、`CMakeLists.txt`。

**Interfaces:**

- `using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>`。
- `Error sqlite_error(sqlite3*, int rc, std::string_view stage)`；忙扩展码按低 8 位识别。
- `Result<Statement> prepare(sqlite3*, std::string_view sql, std::string_view stage)`。
- `Status execute(sqlite3*, const char* sql, std::string_view stage)`。
- `Result<std::int64_t> query_integer(sqlite3*, const char* sql, std::string_view stage)`。
- `enum class TransactionMode { read, write }`；可移动不可拷贝 `Transaction` 提供 `static Result<Transaction> begin(sqlite3*, TransactionMode)`、`Status commit()`，析构回滚活动事务。
- `Status ensure_schema(sqlite3*, OpenMode)`，内部版本常量 `schema_version = 1`。

**Steps:**

- [x] 提取测试夹具，冻结实际历史 DDL；编写新库五表／两索引／版本 1、有效库重开、旧账号迁移、只读旧库拒绝、未知版本和对象／定义差异测试。
- [x] 构建并运行 `store`，观察旧实现对完整版本 1 或旧库迁移的具体行为失败。
- [x] 实现语句和事务辅助；事务提交成功前保持活动，任何提前返回回滚。
- [x] 集中实现五表及索引 DDL；保留历史账号定义；参考内存库生成对象目录，与实际 `(type,name,tbl_name,sql)` 比较，排除 `sqlite_stat1/4`。
- [x] 实现 `ensure_schema`：事务内分类 0／1／不支持版本，验证旧账号记录，迁移并在最后更新版本；只读模式只接受完整版本 1。
- [x] 替换 `sqlite_store.cpp` 中的三个布尔检查函数，接入新生命周期；账号写入复用语句辅助但不改变身份保护逻辑。
- [x] 运行全部现有 CTest，要求全部通过且无编译警告。

**Verification:** `cmake --build build/macos-debug`，然后 `ctest --test-dir build/macos-debug --output-on-failure`；预期退出 0。`store` 独立检查表名、版本、索引和账号保留，不只调用被测校验器。

## Task 2: 配置与失败恢复验收

**Files:** `src/store/sqlite_utils.h/.cpp`、`src/store/sqlite_store.cpp`、`tests/store_test.cpp`、`CMakeLists.txt`。

**Interfaces:** `Status configure_connection(sqlite3*)` 配置扩展返回码、1000ms busy timeout 和外键回读；`Status configure_wal(sqlite3*)` 在事务外对文件库设置并回读 WAL，内存库跳过。

**Steps:**

- [x] 编写并运行连接外键实际拒绝、WAL 回读、1000ms 外部写锁忙错误、损坏文件及 schema、内存数据库、VACUUM／ANALYZE 重开测试。
- [x] 实现连接配置，在 `open` 中按“连接配置→ensure_schema→仅写模式 WAL”装配。
- [x] 使用测试原生连接 authorizer 拒绝第二张表、版本设置、COMMIT 或 WAL，验证阶段、返回码、回滚状态以及后续重试。
- [x] 参数化验证版本 -1、2、INT32_MIN、INT32_MAX 的两种模式，以及有效版本 1 只读不能写入。
- [x] 独立断言外键、远端唯一键、解析状态和附件顺序键等关键约束能实际拒绝非法写入。
- [x] 运行 Debug 全套 CTest，要求全部通过；测试不使用真实网络和邮件数据。

**Verification:** 与 Task 1 相同，预期退出 0；authorizer 失败场景全部检查版本、表、哨兵行和 `sqlite3_get_autocommit`，解除故障后重试成功。

## Task 3: 审阅、Release 和文档收口

**Files:** 上述实现／测试文件；`README.md`、`docs/dependencies.md`、生命周期设计、本计划和阶段 2 状态。

**Steps:**

- [x] 对照设计完成一次独立代码审阅，修复影响数据安全或契约的发现，并为新发现添加有意义的回归测试。
- [x] 新建 `build/verify-sqlite-release` 构建目录，使用 README 的 Homebrew 前缀和本机部署目标配置 Release。
- [x] 运行 Release 构建和全套 CTest，要求退出 0、无编译警告；执行 `git diff --check`。
- [x] 文档记录已实现的数据库生命周期和真实 Debug／Release 验证结果；Task 2 整体保持进行中，不把建表当作邮件存储完成。
- [x] 仅更新本计划勾选状态与一至两句验证结论，交付工作区改动清单；不自动暂存或提交。

**Verification:** `cmake --build build/verify-sqlite-release`；`ctest --test-dir build/verify-sqlite-release --output-on-failure`；`git diff --check`。预期均退出 0。

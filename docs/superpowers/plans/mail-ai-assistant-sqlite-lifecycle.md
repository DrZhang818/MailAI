# SQLite 数据库生命周期实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 收敛为“空版本 0 初始化、版本 1 校验、其他状态拒绝”，删除开发中间数据库的迁移逻辑。

**Architecture:** `SqliteStore::open` 继续协调连接配置、`ensure_schema` 和 WAL。schema 模块只管理当前版本 1 的 DDL、初始化和目录比较；语句与事务使用现有 RAII。

**Scope:** 修改生命周期实现、测试和相关文档；不修改五表数据模型、公开接口，不实现邮件业务读写或通用迁移框架。

**Tech Stack:** C++23、SQLite 3.37+、CMake 3.20+、Ninja、CTest、macOS。

**Spec:** [生命周期设计](../specs/mail-ai-assistant-sqlite-lifecycle-design.md)，隶属[阶段 2](mail-ai-assistant-stage-2.md) Task 2。用户已明确授权按修订后的打开策略直接实施。

**Status:** 已完成本次单版本收敛，账号 DDL 已恢复最初的版本 1 缩进。使用提交 `d0c9410` 创建的数据库采用另一种缩进，当前会被拒绝打开，原结构和数据保留；Task 2 的邮件业务接口、同步队列和进程锁仍未实现。

## Global Constraints

- 在当前共享工作区实施，不回退用户修改，不自动暂存、提交或推送。
- 公共 `SqliteStore::open(path, OpenMode)` 和 `ensure_account` 接口不变；账号 DDL 缩进及其兼容性限制见规格第 4.1 节。
- 版本 0 是未初始化状态；仅空库可以初始化，非空一律拒绝且保留数据；只读模式不初始化。
- `schema_version=1`；不支持的版本和结构返回 `storage_error`，错误阶段为 `storage.schema`；初始化执行错误阶段为 `storage.initialize`。
- 版本设置与建表在同一事务内；保留外键、1000ms busy timeout、WAL 和失败回滚契约，具体行为见规格。

## Review Focus

1. 仅有账号表、已有完整结构但版本为 0，以及陌生表／视图，均不能被自动接管。
2. 版本 1 即使目录为空也必须拒绝，不能进入初始化分支。
3. 删除旧结构测试后，版本 1 的列类型、约束、额外对象检查仍有真实 SQLite 测试覆盖。
4. 建表、版本设置和提交失败后，没有残留结构或活动事务，解除故障可重试。
5. 有效版本 1 的数据、只读打开、外键、WAL、统计信息和锁争用行为不退化。

## Task 1: 单版本生命周期收敛

**Files:** 修改 `src/store/schema.cpp`、`tests/store_test.cpp`、`tests/store_test_support.h`、`CMakeLists.txt`、`README.md`、`docs/dependencies.md`、本计划、生命周期设计和阶段 2 计划；删除 `tests/fixtures/store/legacy_accounts.sql`。

**Interfaces:** 消费现有 `Result<T>`／`Status`、`Transaction::begin`／`commit` 和 SQLite helpers；保留 `Status ensure_schema(sqlite3*, OpenMode)`。内部 `reference_catalog()` 只生成版本 1，不再接受版本参数。

**Steps:**

- [x] 将旧账号迁移测试替换为非空版本 0 拒绝测试，断言两种打开模式均返回 `storage_error`，目录、版本及记录不变。
- [x] 构建并运行 `store`，确认当前实现因自动迁移账号库而违反新契约。
- [x] 删除旧结构识别、旧记录校验和迁移分支；仅在空版本 0 上初始化，统一校验版本 1 目录，最后设置版本并提交。
- [x] 删除旧 SQL 夹具及编译宏，保留通用账号插入辅助；将定义变体测试移到版本 1，补充空目录版本 1 拒绝和非空版本 0 保护场景。
- [x] 保留初始化建表、版本设置和提交故障注入，验证回滚持久状态及恢复重试；更新错误阶段为 `storage.initialize`。
- [x] 运行 Debug／Release 全套 CTest、格式检查和 `git diff --check`，修正相关文档的旧库兼容描述。
- [x] 完成一次独立代码审阅，处理影响契约的发现并记录实际验证结论。

**Verification:**

- `cmake --build build/macos-debug --target store_test` 和 `ctest --test-dir build/macos-debug -R '^store$' --output-on-failure`；最终预期退出 0。
- `cmake --build build/macos-debug` 和 `ctest --test-dir build/macos-debug --output-on-failure`；预期 5/5 通过且无构建警告。
- `cmake --build build/verify-sqlite-release` 和 `ctest --test-dir build/verify-sqlite-release --output-on-failure`；预期 5/5 通过且无构建警告。
- `clang-format --dry-run --Werror src/store/schema.cpp tests/store_test.cpp tests/store_test_support.h` 和 `git diff --check`；预期退出 0。

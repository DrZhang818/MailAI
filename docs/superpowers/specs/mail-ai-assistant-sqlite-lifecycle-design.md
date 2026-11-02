# SQLite 版本 1 与数据库生命周期设计

**状态：** 用户已批准并按[实施计划](../plans/mail-ai-assistant-sqlite-lifecycle.md)交付。独立代码审阅未发现缺陷；macOS Debug／Release 各 5 项 CTest 通过。此状态只覆盖数据库生命周期，不代表 Task 2 整体完成。

**目标：** 为阶段 2 Task 2 建立可维护的数据库打开、初始化、旧库迁移和结构校验流程。一次成功的读写打开得到完整版本 1；失败能够定位，并保留已有业务数据。

**范围：** 五张表及必要索引的定义、连接配置、版本识别、事务、结构校验和对应测试。邮件查询、同步队列推进、进程锁仍按阶段 2 后续步骤实现；本次不实现收信、MIME 或 AI。

**技术栈：** C++23、SQLite 3.37+、CMake 3.20+、Ninja、CTest、macOS。沿用 `Result<T>`／`Status` 与现有 `SqliteStore` 公共接口，无新运行时依赖。

**上位文档：** [整体设计](mail-ai-assistant-design.md) 第 2、3、7 节；[阶段 2 计划](../plans/mail-ai-assistant-stage-2.md) Task 2。本文细化其 SQLite 生命周期；后续实施计划引用本文，不复制全部设计。

## 1. 现状与决策

当前代码可以管理 SQLite 连接并保存账号，创建的数据库未设置版本号，只有 `accounts` 表。逐步追加的表名、列名、类型和约束查询尚未构成完整版本管理；也没有事务初始化、完整版本 1 或迁移流程。

保留连接／语句 RAII、账号保存接口和有效行为测试。将版本协调、DDL 和结构校验移出 `sqlite_store.cpp`，替换不断增长的布尔 SQL；测试提取公共夹具。已有未提交修改保留，不重置工作区。

### 方案选择

| 方案 | 取舍 | 决定 |
|---|---|---|
| 五张表统一作为版本 1 | 必须先完成数据模型，但初始化、迁移和后续存储接口共享明确基线 | 用户已选择 |
| 账号表先作为版本 1，后续逐表升级 | 首个交付较小，但需要修改原阶段计划并增加迁移路径 | 不采用 |

下文的目录比较方式、只读旧库行为和 SQLite 等待时间已随本文确认。

## 2. 职责与代码边界

| 文件 | 职责 |
|---|---|
| `include/mailai/store.h` | 保留 `SqliteStore::open(path, mode)`、RAII 与业务存储接口，不暴露原生连接或 schema 细节 |
| `src/store/sqlite_store.cpp` | 协调连接打开、连接配置、schema 准备、日志模式配置；保留账号业务写入 |
| `src/store/schema.h`、`src/store/schema.cpp` | 内部 `ensure_schema(sqlite3*, OpenMode)`；版本常量、固定 DDL、结构目录比较、空库初始化和已知旧库迁移 |
| `src/store/sqlite_utils.h`、`src/store/sqlite_utils.cpp` | 内部语句所有权、SQL 执行、错误转换和事务 RAII；不形成通用数据库框架 |
| `tests/store_test_support.h` | 仅测试使用的临时目录、原生测试连接、SQL 执行和标量查询辅助 |
| `tests/fixtures/store/legacy_accounts.sql` | 独立冻结已交付的旧账号 DDL，作为版本 0 迁移的正向夹具，不能从被测代码动态生成 |
| `tests/store_test.cpp` | 以场景／参数表验证完整行为；继续注册为 CTest 的 `store` |

`schema` 只依赖 SQLite、公共值类型和内部辅助。它不调用账号保存业务、不读取环境或密码、不输出日志。业务存储方法不得绕过已成功的 schema 准备。

事务对象不拥有连接，必须先于连接析构。成功 `COMMIT` 前始终处于活动状态；退出活动事务时尝试 `ROLLBACK`。提交失败仍保持可回滚状态。析构不抛异常、不覆盖原始失败信息；显式操作通过 `Status` 报错。

## 3. 版本及打开行为

当前正式 schema 版本为 `1`。`PRAGMA main.user_version` 是版本标记，不能单独证明结构有效。负数和大于 1 的版本一律拒绝。

| 文件／版本／结构 | `read_only` | `read_write_create` |
|---|---|---|
| 文件不存在 | `storage_error`，不创建文件 | 父目录已存在时创建并初始化版本 1 |
| 版本 0，没有应用结构对象 | `storage_error`，提示需要初始化 | 在事务中创建完整版本 1 |
| 版本 0，只有已知旧版 `accounts` | `storage_error`，提示需要迁移 | 验证旧结构及账号记录后迁移到版本 1 |
| 版本 0，其他结构 | `storage_error`，保留原数据 | 相同行为 |
| 版本 1，完整匹配本项目定义 | 成功，只读取并验证 | 成功，不重建已有表 |
| 版本 1，缺表、缺索引或存在结构差异 | `storage_error` | 相同行为，不用 `IF NOT EXISTS` 静默修补 |
| 版本不受支持或 SQLite 报告损坏 | `storage_error` | 相同行为，不删除后重建 |

父目录由运行时装配负责，`SqliteStore::open` 不创建目录。CLI 对“不存在的缓存返回空列表”的处理仍由阶段 2 Task 6 负责，不依靠存储层创建空库。

只读旧库不在后台迁移，也不假装已有完整邮件缓存。只有显式读写打开可以初始化或迁移。当前把版本 1 全部视为“尚未实现”的临时测试，要替换为“有效版本 1 可打开、结构不匹配的版本 1 被拒绝”。

## 4. 版本 1 数据模型

以下 `INTEGER` 使用 SQLite 有符号 64 位整数，`TEXT` 使用应用拥有的字符串，原始邮件使用 `BLOB`。除标明可空的字段外，所有字段显式 `NOT NULL`。不使用 STRICT 表或 WITHOUT ROWID，避免改变既有账号表格式。字段名与后续存储方法共用本契约。

### 4.1 accounts

保持提交 `c0c537b` 中应用创建的账号表定义，八列、顺序及声明不变，以便迁移时保留原表和数据。

| 字段 | 类型及约束 |
|---|---|
| `id` | `TEXT PRIMARY KEY NOT NULL` |
| `display_name`、`address`、`imap_host` | `TEXT NOT NULL` |
| `imap_port` | `INTEGER NOT NULL` |
| `imap_username`、`credential_env`、`tls_mode` | `TEXT NOT NULL` |

`ensure_account` 继续校验必填字段、非零端口及 TLS 枚举，拒绝同账号的主机／端口／用户名变化；只存凭据环境变量名。版本 1 不顺带改变该表的约束。

旧库迁移前检查账号记录：八列值的存储类型符合声明；`id`、`address`、`imap_host`、`imap_username`、`credential_env` 非空；`imap_port` 为 1–65535；`tls_mode` 为 `implicit` 或 `starttls`。`display_name` 可为空字符串。发现无效记录返回 `storage_error`，不清洗或删除数据；诊断不输出字段内容。

### 4.2 sync_state

| 字段 | 类型及约束 |
|---|---|
| `account_id`、`folder` | `TEXT`；联合主键；账号外键引用 `accounts(id)`，删除规则为 RESTRICT；folder 非空 |
| `uid_validity` | 可空 `INTEGER`；非空时范围 1–4294967295 |
| `last_discovered_uid` | `INTEGER DEFAULT 0`，范围 0–4294967295 |
| `last_attempt_at`、`last_success_at` | 可空 `INTEGER`，UTC epoch 秒 |

无 UIDVALIDITY 时 `last_discovered_uid` 必须为 0。每个账号／文件夹最多一行；首次调度负责建立记录，账号初始化不伪造同步进度。

### 4.3 sync_pending

| 字段 | 类型及约束 |
|---|---|
| `account_id`、`folder` | `TEXT`；联合外键引用 `sync_state(account_id, folder)`，删除规则为 RESTRICT |
| `uid_validity`、`uid` | `INTEGER`，范围 1–4294967295 |
| `retryable` | `INTEGER DEFAULT 1`，只允许 0 或 1 |
| `last_error_code` | 可空 `TEXT`；保存稳定的 `ErrorCode` 名称，不保存枚举序号 |
| `last_error_stage`、`last_error_message` | `TEXT DEFAULT ''` |

主键为 `(account_id, folder, uid_validity, uid)`。无错误时 code 为 NULL、stage/message 为空；读写错误类型时使用明确的字符串转换，未知名称作为存储错误处理。`retryable=0` 保存已经确认不应重试的任务；`pending` 只返回可重试项。UIDVALIDITY 更换时事务内移除旧队列，邮件历史单独保留。

### 4.4 messages

| 字段 | 类型及约束 |
|---|---|
| `id` | `INTEGER PRIMARY KEY AUTOINCREMENT`，生成的本地稳定 ID |
| `account_id` | `TEXT`，外键引用 `accounts(id)`，删除规则为 RESTRICT |
| `folder` | 非空 `TEXT` |
| `uid_validity`、`uid` | `INTEGER`，范围 1–4294967295 |
| `sender`、`subject`、`body` | `TEXT` |
| `received_at_utc` | `INTEGER`，UTC epoch 秒 |
| `raw` | `BLOB`，保存原始字节，允许包含 NUL |
| `parse_state` | `TEXT`，只允许 `parsed` 或 `failed` |
| `parse_error` | `TEXT DEFAULT ''` |
| `historical` | `INTEGER DEFAULT 0`，只允许 0 或 1 |
| `message_id`、`in_reply_to`、`references_header` | `TEXT DEFAULT ''`；最后一列映射 `Mail.references` |

远端唯一约束为 `(account_id, folder, uid_validity, uid)`。成功解析要求 `parse_error=''`；失败解析要求诊断非空、`subject='(解析失败)'` 且 `body=''`。这些约束写入固定 DDL；原文存储不依赖解析成功。

显式索引仅设置 `messages_inbox_order(received_at_utc DESC, id ASC)` 和 `messages_account_order(account_id, received_at_utc DESC, id ASC)`。历史邮件和解析失败邮件保留在查询范围，状态由业务展示，不创建额外历史表。

### 4.5 attachments

| 字段 | 类型及约束 |
|---|---|
| `mail_id` | `INTEGER`，外键引用 `messages(id)`，删除规则为 CASCADE |
| `ordinal` | `INTEGER`，非负，保留附件在向量中的顺序 |
| `name`、`media_type` | `TEXT` |
| `size_bytes` | `INTEGER`，非负 |

主键为 `(mail_id, ordinal)`。`AttachmentInfo.size_bytes` 超过 `INT64_MAX` 时，后续 `complete` 应在绑定和写事务前返回 `invalid_argument`。`MailId` 绑定的范围校验同样保留在相应业务接口，不能靠隐式转换。

## 5. 结构校验：固定应用格式，不实现 SQL 解析器

本项目只兼容应用自己创建的版本结构和已知旧账号结构，不承诺接受任意手工编写的等价 DDL。

各版本 DDL 在 `schema.cpp` 中只有一份权威定义：旧账号定义固定保存；版本 1 复用同一账号定义并增加四表及两索引。变更已发布版本的 DDL 必须升级版本，不能直接改常量绕过迁移。

校验时在独立的临时内存连接执行相应版本的权威 DDL，由当前 SQLite 生成参考目录。读取实际和参考 `main.sqlite_schema`，在 C++ 中按 `(type, name)` 排序后比较 `type, name, tbl_name, sql`；不比较依赖物理布局的 `rootpage`。不再分别维护一份列数量、列名和类型白名单，也不自行编写 SQL 格式化或语义等价算法。

比较包含应用表、显式索引、视图、触发器、自动索引和版本 1 的 `sqlite_sequence` 定义。额外的视图／触发器／索引、CHECK、UNIQUE、默认值、排序规则或生成列都会形成目录差异而被拒绝。`sqlite_sequence` 的行值不与空参考库比较；它不是结构定义。SQLite 的统计表 `sqlite_stat1`／`sqlite_stat4` 如存在则排除，其内容不参与应用格式识别。

只接受已知 DDL 的存储形式是一项有意的兼容性限制：即使列看起来相同，手工改写过的 SQL 文本也可能被拒绝。需要兼容新的已知格式时，添加命名的版本迁移或已知旧格式，而不是越来越宽松地猜测。正常 VACUUM、重新打开和统计信息更新不能改变校验结论。

发现首个差异时诊断包含对象名称和“缺失／额外／定义不符”，不输出原始 SQL 或记录值。参考库的创建失败同样返回 `storage_error`，不能跳过校验。

这里验证应用结构，不承诺检测任意文件位损坏或恶意 `writable_schema` 篡改。SQLite 在打开和查询过程中报告的损坏必须传播；不在每次打开时扫描全部邮件内容运行完整性检查。

## 6. 连接、事务与日志模式

### 6.1 公共连接配置

打开原生连接后立即纳入现有 RAII 所有权。设置扩展返回码、1000 毫秒 SQLite busy timeout，并执行／回读 `PRAGMA foreign_keys=ON`；回读必须为 1。该连接设置在 schema 事务之前执行。读写两种连接均配置外键，但只读连接不切换日志模式、不修改磁盘结构。

`SQLITE_BUSY`／`SQLITE_LOCKED`（包括对应扩展码）统一映射为 `ErrorCode::busy`；其他 SQLite 执行错误映射为 `storage_error`。SQLite busy timeout 和后续数据目录进程锁的默认 5 秒等待属于不同层，后者仍由 Task 2 的锁步骤实现。

### 6.2 读写打开

1. `BEGIN IMMEDIATE`，在同一事务快照内读取版本及结构并分类。不得先在无事务状态下校验，再依赖过时结果写入。
2. 空版本 0：执行五表及索引 DDL，校验版本 1 目录，最后设置 `user_version=1`。
3. 已知账号版 0：验证结构与账号记录，保留原 `accounts` 表，创建其余四表及索引，校验完整目录，最后设置 `user_version=1`。
4. 版本 1：校验结构，不自动补表、补索引或重建。
5. 不支持版本、陌生结构或上述任何步骤失败：回滚，原版本、表和已有行保持不变。
6. 成功提交后，对于文件数据库设置并回读 `journal_mode=WAL`；实际返回必须为 `wal`。内存数据库不要求 WAL。

版本号更新与建表／迁移处于同一事务，不能出现版本已更新但缺表的中间持久状态。新文件初始化失败可以留下空的 SQLite 文件，但不得留下半套应用表；已有文件不删除、不替换。

### 6.3 WAL 的独立失败边界

SQLite 不允许在活动事务内切换 WAL。因此 WAL 配置在 schema 提交之后执行；未知数据库会在更早的结构检查中被拒绝，不会主动切换它的日志模式。

若 schema 已提交、随后 WAL 配置失败，`open` 返回错误，但数据库保留一个完整有效的版本 1 和全部原有数据。下一次读写打开可再次尝试 WAL；只读打开可以读取这个有效版本 1，即使它暂时仍使用原日志模式。不能声称这种失败把版本回滚为 0。只有成功的文件数据库读写打开才保证使用 WAL。

schema 事务失败保证逻辑结构／版本／记录回滚，不保证已有文件逐字节不变；SQLite 可以生成日志或修改内部文件布局。失败验收应查询持久状态。对根本不是 SQLite 的文件，打开拒绝且原字节保留。

### 6.4 只读打开

使用 `SQLITE_OPEN_READONLY`，在只读事务中读取版本和结构，避免多次校验跨越不同快照；仅完整有效的版本 1 成功。事务结束后返回只读存储对象，不触发迁移、建表、版本写入或 WAL 切换。读取 WAL 数据库时 SQLite 可能访问其标准辅助文件；“只读”契约是不修改业务状态和 schema，不承诺不存在辅助文件访问。

## 7. 错误与恢复契约

| 情况 | code | stage |
|---|---|---|
| 原生连接打开失败 | `storage_error`，忙错误除外 | `storage.open` |
| 不支持版本、未知结构、目录差异、只读库需要初始化／迁移 | `storage_error` | `storage.schema` |
| 旧账号记录不合法或迁移 DDL／版本写入失败 | `storage_error`，忙错误除外 | `storage.migrate` |
| BEGIN／COMMIT 失败 | `storage_error`，忙错误除外 | `storage.transaction` |
| 外键、超时或 WAL 配置／回读失败 | `storage_error`，忙错误除外 | `storage.configure` |
| 任意步骤的 SQLite 忙错误 | `busy` | 保留发生步骤对应 stage |

保留最初失败；回滚清理不把它替换成另一个错误。诊断不包含凭据值、真实邮件正文、绑定参数或账号记录内容。schema 打开失败时不返回部分可用的 `SqliteStore`。

## 8. 验收与测试组织

测试继续使用临时文件和真实 SQLite。公共夹具封装语句释放、SQL 执行和标量读取，变体结构用参数表表达；不为每个变体复制整套连接和查询代码。既有正常账号保存、身份保护和范围测试保留。

| 场景 | 必须观察的结果 |
|---|---|
| 新建及已有空版本 0 | 五表、两显式索引、版本 1；重开及账号保存成功 |
| 已知旧账号库迁移 | 原账号值、行数和主键保留，新增四表为空，版本为 1；含引号和中文的虚构值不变 |
| 已有有效版本 1 | 只读／读写均成功，重开不丢记录、不重建结构 |
| 版本 -1、2、INT32_MIN、INT32_MAX | 两种模式均拒绝，原版本、表及虚构哨兵记录不变 |
| 只读版本 0／文件不存在 | 返回明确存储错误；不创建应用表或写入版本 |
| 缺表／缺索引／额外表、视图、触发器、索引 | 拒绝；原结构和记录保留；包含阻止账号插入的触发器回归 |
| 列类型、主键、NOT NULL、额外列、CHECK、UNIQUE、默认值或排序规则有差异 | 拒绝并指向相关对象；不清洗或修复 |
| 非 SQLite 文件／损坏的 schema | 两种模式按实际 SQLite 诊断失败，非 SQLite 原文件字节保留 |
| 建第二张表失败／设置版本失败／COMMIT 失败 | 通过测试连接的 SQLite authorizer 或可控锁制造失败；已有账号和版本不变，无部分新增表 |
| 持有外部写事务 | busy 在配置的等待边界内返回，释放外部事务后可重试成功 |
| schema 成功但 WAL 被拒绝 | 返回配置错误，仍是完整版本 1；解除故障后可重新打开 |
| 外键和 WAL 回读 | 使用内部测试连接验证实际开关和真实外键拒绝行为；不只检查 PRAGMA 执行返回码 |

失败注入只配置测试拥有的原生连接并调用内部 helper，不向业务 API 增加测试开关，不依赖真实磁盘写满或不可移植的权限假设。参考 schema 不能作为唯一验收依据：测试还要独立断言表名、关键外键／唯一约束、账号保留、版本和事务失败结果。

生命周期交付需要 Debug 和新 Release 构建目录的全套 CTest 均通过，并检查编译警告。实际命令、任务文件和执行顺序由审批后的实施计划给出。当前测试通过记录不等于本设计已实现；Task 2 的邮件读写、同步待办和写进程锁仍单独验收。

## 9. 依据与限制

- [SQLite user_version](https://www.sqlite.org/pragma.html#pragma_user_version)：版本整数由应用解释。
- [SQLite schema table](https://www.sqlite.org/schematab.html)：对象目录、存储的创建语句和 SQLite 的有限规范化规则。
- [SQLite transactions](https://www.sqlite.org/lang_transaction.html)：事务、快照、写锁及提交失败行为。
- [SQLite foreign_keys](https://www.sqlite.org/pragma.html#pragma_foreign_keys)、[journal_mode](https://www.sqlite.org/pragma.html#pragma_journal_mode)：外键需显式配置，WAL 切换具有独立事务限制。

本设计不引入 ORM、通用迁移框架、SQL 解析器或新的外部依赖。对固定应用格式做明确校验；后续新增结构通过版本迁移演进。

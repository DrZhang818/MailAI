# Mail AI Assistant Stage 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 接入一个真实学校邮箱，将收件箱邮件解析并保存到 SQLite，支持离线查看、重复同步去重和中断后继续。

**Architecture:** 保留 CLI → 业务服务 → 邮件／存储的依赖方向。同步服务串行调用 IMAP、MIME 和 SQLite；查询命令只读取本地缓存。持久待处理队列记录已发现但尚未完成的 UID，防止失败邮件被同步进度越过。

**Tech Stack:** C++23、CMake 3.20+、Ninja、CTest、WSL Ubuntu；libcurl 8.0+、SQLite 3.37+、GMime 3.2+、libxml2 2.9+。Python 3 与 OpenSSL 命令行只用于本机 TLS 测试服务器和测试证书。

**Spec:** [已批准的整体设计](../specs/mail-ai-assistant-design.md)，本计划细化第 3、7、8 节与第 9 节阶段 2。[阶段 1](mail-ai-assistant-stage-1.md) 已完成，代码基线为 `f4948ab`。

**Status:** Task 1 已完成并提交为 `874e0a6`。Task 2 进行中：基础账号配置存储已实现，Debug 构建无警告，5 项 CTest 全部通过；数据库版本管理和同步状态待实现。阶段 2 尚未验收。

## Global Constraints

- 按用户要求先说明下一项任务的目标、输入输出、约束和验收条件；用户先讲方案，导师点评后再给提示或参考代码，最后带写和验证。每轮聚焦一个小任务。
- 测试、测试夹具和验证默认由导师维护，用户可参与；只有用户明确委托时才直接修改业务源码。
- 使用 C++23；核心库公开 `cxx_std_23`，所有项目目标设置 `CXX_STANDARD 23`、`CXX_STANDARD_REQUIRED ON`、`CXX_EXTENSIONS OFF`。
- 构建、测试和运行通过 `wsl.exe -d Ubuntu -- ...` 调用 WSL 工具。
- 一个可执行程序 `mail_assistant` 调用一个核心库 `mailai_core`；持久记录拥有自己的字符串和容器，句柄通过 RAII 管理。
- 首个接入对象为学校邮箱，默认账号 ID 为 `school`。主机、端口和用户名按本人设置页填写，不硬编码未实测的服务器参数。
- 凭据值只放在本地环境变量，数据库只保存凭据引用，日志排除凭据。使用学校客户端专用密码，用户无需把密码发到对话中。
- 本阶段只收取 `INBOX`：首次取最高 UID 对应的最近 20 封，后续获取新邮件和仍待重试的邮件；使用 `BODY.PEEK[]`，保持服务器已读状态。
- 网络默认连接超时 10 秒、总超时 30 秒；临时连接错误最多重试 2 次，退避 1、2 秒；认证、证书校验、参数和解析错误直接报告。
- 正常结果写 stdout，进度和诊断写 stderr。退出码：0 全部成功，1 全部失败或无法执行，2 参数错误，3 部分成功；空邮箱和无新邮件返回 0。
- 本阶段不实现 AI、SMTP、回复、多账号管理、watch 和 JSON 输出。

## Review Focus

1. 较小 UID 失败、较大 UID 成功后重启：失败项仍会重试，成功项不重复。Task 2、5 覆盖。
2. UIDVALIDITY 改变，或抓取期间服务器标识范围变化：历史保留并标记，旧范围任务不用于新邮件。Task 2、4、5 覆盖。
3. `uint64_t` 邮件 ID 超过 SQLite 有符号范围：查询返回未找到，禁止截断或变成负数。Task 2、6 覆盖。
4. 中文、HTML、multipart、附件和损坏邮件混合：正文正确，失败项保留原文与索引且不中断其他项。Task 3、5 覆盖。
5. 未配置凭据时离线查看，以及无效参数、重复 sync、数据库写失败：查询不连接网络，参数错误先返回，失败不破坏缓存。Task 1、2、6 覆盖。

## Scope Decisions

| 事项 | 决定 |
|---|---|
| 数据来源 | 默认读取真实本地缓存；阶段 1 样例只在显式 `--demo` 时使用 |
| MIME | GMime 负责 MIME 和字符集转换，libxml2 负责 HTML 转文本 |
| 依赖 | 使用 WSL 系统开发包和已固定的最低版本；实际版本记录在 `docs/dependencies.md` |
| 首次同步 | 注册最近 20 个 UID；更早邮件本阶段不回填 |
| 增量方式 | 扫描 UID 列表并在本地筛选；只下载新 UID 和待重试项，先保证正确性 |
| 正文取舍 | `multipart/alternative` 优先 `text/plain`，缺失时将 `text/html` 转文本 |
| 附件 | 只保存名称、媒体类型和大小，不作为正文 |
| 远端变化 | 不删除邮件、不修改已读状态；本地历史不因远端消失而删除 |

## Commands and Configuration

| 命令 | 行为 |
|---|---|
| `mail_assistant --help` | 显示帮助，不打开数据库或连接网络 |
| `mail_assistant sync --account school` | 同步配置账号，输出成功／失败计数，失败定位到 UID |
| `mail_assistant inbox [--account school]` | 只读本地列表，显示“数据来源：本地缓存” |
| `mail_assistant show <id>` | 只读本地详情，显示附件元数据及解析／历史状态 |
| `mail_assistant --demo inbox` | 阶段 1 固定样例，显示“数据来源：示例” |
| `mail_assistant --demo show 43` | 查看固定样例详情 |

`--demo` 只允许出现在开头；`--demo sync` 返回参数错误 2。无参数仍显示帮助，`inbox --account all` 保留。

| 变量 | 规则 |
|---|---|
| `MAILAI_DATA_DIR` | 可选；缺省为 `$XDG_DATA_HOME/mail-ai-assistant`，再缺省为 `$HOME/.local/share/mail-ai-assistant`；相对 XDG 路径被忽略 |
| `MAILAI_ACCOUNT_ID`、`MAILAI_ACCOUNT_NAME` | 可选，缺省 `school`、`学校邮箱` |
| `MAILAI_EMAIL`、`MAILAI_IMAP_HOST`、`MAILAI_IMAP_USER` | sync 必填，拒绝空值 |
| `MAILAI_IMAP_TLS` | `implicit`（默认）或 `starttls`，均验证证书和主机名 |
| `MAILAI_IMAP_PORT` | 可选；implicit 缺省 993，starttls 缺省 143；十进制 1–65535 |
| `MAILAI_PASSWORD_ENV` | 可选，缺省 `MAILAI_IMAP_PASSWORD`；数据库只保存该变量名 |
| `MAILAI_IMAP_PASSWORD` 或指定变量 | 只在本机设置；仅在创建连接时读取 |

`inbox`、`show` 只需要数据目录，不要求 IMAP 配置或密码。数据库不存在时返回 0 封，不自动展示样例。

## Files and Interface Contract

路径均相对于项目根目录。

| 文件 | 责任 | Task |
|---|---|---|
| `include/mailai/model.h`、`include/mailai/inbox.h`、`src/app/inbox.cpp` | 解析／历史状态、附件信息和列表投影 | 1 |
| `include/mailai/config.h`、`src/config.cpp` | 数据目录与连接参数校验 | 1 |
| `include/mailai/sync_types.h`、`include/mailai/mime.h` | 服务器定位、同步值和解析值类型 | 1 |
| `include/mailai/store.h`、`src/store/sqlite_store.cpp` | SQLite schema、查询、事务和待处理项 | 2 |
| `include/mailai/data_dir_lock.h`、`src/store/data_dir_lock.cpp` | 单数据目录写进程锁 | 2 |
| `src/mail/mime.cpp`、`src/mail/html_text.h`、`src/mail/html_text.cpp` | MIME、字符集和 HTML 正文提取 | 3 |
| `include/mailai/imap.h`、`src/mail/imap.cpp` | libcurl IMAP 适配、TLS、超时和重试 | 4 |
| `include/mailai/sync.h`、`src/app/sync.cpp` | 持久队列和单账号同步编排 | 5 |
| `src/cli.h`、`src/cli.cpp` | CLI 参数、显示和退出码 | 6 |
| `src/runtime.h`、`src/runtime.cpp`、`src/main.cpp` | 装配真实／样例服务，按需打开资源 | 6 |
| `tests/config_test.cpp`、`tests/store_test.cpp` | 配置和真实临时 SQLite 测试 | 1、2 |
| `tests/mime_test.cpp`、`tests/fixtures/mail/*.eml` | 虚构 MIME 样本与期望值 | 3 |
| `tests/imap_test.cpp`、`tests/imap_server.py` | 实际 libcurl 对接本机 TLS 服务器 | 4 |
| `tests/sync_test.cpp` | 可控收信结果＋真实 SQLite 的恢复测试 | 5 |
| `tests/cli_test.cpp`、`tests/runtime_test.cpp`、`tests/cli_smoke.cmake` | CLI、缓存和 sync 验收 | 6 |
| `tests/live_mailbox_test.cpp` | 显式运行的真实收信探针，不进入默认 CTest | 7 |
| `CMakeLists.txt`、`README.md`、`docs/dependencies.md` | 构建、命令和实际验证记录 | 各任务随交付更新 |

值类型及字段：

| 类型 | 字段 |
|---|---|
| `AttachmentInfo` | `string name, media_type; uint64_t size_bytes{}` |
| `ParseState` | `parsed, failed` |
| `Mail` 增量字段 | `folder`、`uid_validity`、`uid`、`parse_state{parsed}`、`parse_error`、`historical{false}`、`message_id`、`in_reply_to`、`references`、`attachments` |
| `InboxItem` 增量字段 | `parse_state{parsed}`、`historical{false}` |
| `TlsMode` | `implicit, starttls` |
| `ImapSettings` | `host, username, credential_env`；`uint16_t port{993}`；`TlsMode`；连接／总超时 10／30 秒；可选 `ca_file` |
| `RuntimeConfig` | `Account account; ImapSettings imap; filesystem::path data_dir`，不含密码 |
| `RemoteKey` | `account_id, folder, uid_validity, uid`；非空账号和文件夹，两个服务器数值必须非零 |
| `MailboxSnapshot` | `uid_validity` 和 UID 列表，本阶段固定 `INBOX` |
| `FetchedMail` | `RemoteKey key; int64_t received_at_utc; string raw`，时间取服务器 INTERNALDATE |
| `ParsedMail` | 发件人、主题、正文、Message-ID、回复关联头和附件元数据 |
| `SyncState` | 可空 UIDVALIDITY、最后发现 UID、最后尝试／成功时间 |
| `SyncFailure` | `RemoteKey key; Error error` |
| `SyncSummary` | 发现、成功、失败计数；`reindexed`；逐项失败列表 |

所有新增成员提供默认值并保留原字段顺序，使阶段 1 的聚合初始化仍可用。`Mail.id` 默认 0。`ErrorCode` 增加 `config_error, auth_error, network_error, protocol_error, remote_gone, parse_error, storage_error, busy`；`Error` 末尾增加默认空的 `stage`。

解析失败行使用主题 `(解析失败)`、空正文和非空诊断；成功解析但缺主题时使用 `(无主题)`。references 暂存展开后的原始头字段，原始邮件始终保留；会话归并留到后续阶段。

```cpp
using Environment = std::map<std::string, std::string>;

Result<std::filesystem::path> resolve_data_dir(const Environment& env);
Result<RuntimeConfig> parse_sync_config(const Environment& env);
Result<ParsedMail> parse_message(std::string_view raw);

enum class OpenMode { read_only, read_write_create };

class SqliteStore {
public:
    static Result<SqliteStore> open(const std::filesystem::path&, OpenMode);
    Status ensure_account(const Account&, const ImapSettings&);
    Result<std::vector<InboxItem>> list(const InboxQuery&);
    Result<Mail> find(MailId);
    Result<SyncState> state(const AccountId&, std::string_view folder);
    Status schedule(const AccountId&, std::string_view folder,
                    const MailboxSnapshot&, std::size_t initial_limit);
    Result<std::vector<RemoteKey>> pending(const AccountId&, std::string_view folder);
    Result<MailId> complete(const FetchedMail&, const Result<ParsedMail>&);
    Status fail(const RemoteKey&, const Error&);
    Status record_outcome(const AccountId&, std::string_view folder,
                          const Result<SyncSummary>&, std::int64_t attempted_at);
};

class DataDirLock {
public:
    static Result<DataDirLock> acquire(const std::filesystem::path&,
                                      std::chrono::milliseconds wait);
};

struct ImapSource {
    std::function<Result<MailboxSnapshot>()> snapshot;
    std::function<Result<FetchedMail>(const RemoteKey&)> fetch;
};
Result<ImapSource> make_imap_source(const ImapSettings&, std::string secret);

struct SyncOptions { std::size_t initial_limit{20}; };
Result<SyncSummary> sync_account(SqliteStore&, const Account&,
                                 const ImapSource&, const SyncOptions&);
```

`SqliteStore` 与 `DataDirLock` 均为 RAII 类型，禁止拷贝、允许移动。`make_imap_source` 返回的回调必须拥有会话状态，不捕获已销毁的局部变量。测试用固定结果替换网络边界；存储和 MIME 使用真实实现。

## Task 1: 配置、值类型与依赖基础

**Status:** 已完成，提交 `874e0a6`。

**Files:** `include/mailai/model.h`、`include/mailai/config.h`、`include/mailai/sync_types.h`、`include/mailai/mime.h`、`include/mailai/inbox.h`、`src/config.cpp`、`src/app/inbox.cpp`、`tests/config_test.cpp`、`tests/inbox_test.cpp`、`CMakeLists.txt`、`docs/dependencies.md`、`README.md`。

**Interfaces:** 消费阶段 1 的 `Account`、`Mail`、`Error`、`Result`；产出上述配置、同步和解析值类型，以及 `resolve_data_dir`、`parse_sync_config`。

**Steps:**

- [x] 编写配置与列表状态测试：显式数据目录、XDG、HOME、相对 XDG、缺失和空值回退；默认／自定义账号，implicit／starttls，默认端口、1／65535 边界，以及 16 组非法配置；列表保留解析失败和历史标记。
- [x] 实现 `resolve_data_dir`：只接受绝对 XDG 路径，按专用目录→XDG→HOME 回退；无合法来源时返回 `config_error`。
- [x] 实现 `parse_sync_config`：必填字段拒绝缺失或空值；可选字段为空时用默认值；端口先按无符号整数完整解析再检查 1–65535；配置只保存凭据变量名。
- [x] 将 `Result` 迁移为 `std::expected`，扩展邮件、错误和同步值类型；内存列表投影复制 `parse_state` 和 `historical`。
- [x] 接入 SQLite、libcurl、GMime、libxml2 的 CMake 目标，验证版本与 libcurl 的 IMAP／IMAPS／TLS 能力，并记录实际版本。

**Acceptance:**

- `config` 测试覆盖上述目录、字段、端口和非法输入；`inbox` 测试覆盖状态保留及固定顺序。
- 独立头文件／接口编译检查通过，Debug 构建无警告，`inbox`、`cli`、`config`、`cli_smoke` 共 4/4 通过。
- `docs/dependencies.md` 记录四个依赖的实际版本和已验证范围；不把依赖接入等同于邮件解析或真实收信已验证。

## Task 2: SQLite 缓存与可靠写入

**Files:** `include/mailai/store.h`、`src/store/sqlite_store.cpp`、`include/mailai/data_dir_lock.h`、`src/store/data_dir_lock.cpp`、`tests/store_test.cpp`；必要时扩展 `tests/inbox_test.cpp`。

**Interfaces:** 消费 Task 1 的类型；产出 `SqliteStore` 和 `DataDirLock`，供同步与 CLI 装配调用。

**Steps:**

- [ ] 编写真实临时数据库测试：重开后内容和 ID 保留；相同 `RemoteKey` 重试 ID 不变；已知空账号成功、未知账号报错；写入失败不丢已有邮件或 pending；排序固定为 `42,43,44,7`，其中 `id=7/time=50` 防止仅按 ID 排序也通过。
- [ ] 运行 `store`，确认实现缺失时因具体行为断言失败；检查实际行数、查询内容和待处理项，不只检查“打开成功”。
- [ ] 建立 schema：`accounts`、`sync_state`、`sync_pending`、`messages`、`attachments`；`user_version=1`，外键开启，文件数据库使用 WAL，短事务和参数绑定；不支持或损坏的版本返回错误，禁止删除后重建。
- [ ] 实现查询与写入：本地 ID 为 `INTEGER PRIMARY KEY AUTOINCREMENT`，远端唯一键为 `(account_id,folder,uid_validity,uid)`；禁止用 REPLACE 破坏已有 ID；原文存 BLOB，正文和头字段存 TEXT。
- [ ] 实现同步状态：`schedule` 在同一事务注册 pending 和发现进度；`complete` 在同一事务保存邮件／附件，只有解析成功才移除待办；`fail` 保存抓取原因，`remote_gone` 标记为不可重试；UIDVALIDITY 改变时旧邮件标记历史、旧队列失效。
- [ ] 在 `complete` 统一校验远端定位与解析状态：账号、文件夹非空且 UIDVALIDITY／UID 非零；成功解析清空 `parse_error`，失败解析保存原文、非空诊断和待重试项，并使用约定失败主题与空正文；无效输入不修改已有记录或待办。阶段 1 样例 `Mail` 可以没有远端定位。
- [ ] 实现 `DataDirLock`：写命令获取数据目录独占锁，默认最多等待 5 秒；读缓存不拿写锁；网络请求期间不持有 SQLite 写事务。

**Acceptance:**

- 重开数据库、去重、空账号、未知账号、事务失败恢复和待处理项行为通过。
- `complete` 对成功／失败解析形成正确的持久状态；无效远端定位不修改已有邮件或待办，失败解析保留原文、诊断和重试项。
- 相同账号下服务器、端口或用户名改变被拒绝且原数据保留；显示名可以更新。
- 不同文件夹或 UIDVALIDITY 下相同 UID 不冲突；`UINT64_MAX` 查询返回 `mail_not_found`，不发生有符号截断。
- `store` 和全套 CTest 通过；双写锁争用时受控失败，释放后可恢复。
- 只暂存本任务文件，提交 `feat: persist mail cache and sync work`。

## Task 3: 中文 MIME 与 HTML 正文解析

**Files:** `src/mail/mime.cpp`、`src/mail/html_text.h`、`src/mail/html_text.cpp`、`tests/mime_test.cpp`、`tests/fixtures/mail/*.eml`。

**Interfaces:** 消费原始 RFC 邮件字节，产出 `Result<ParsedMail>`；附件大小为解码后字节数。

**Steps:**

- [ ] 创建虚构邮件样本和测试：plain UTF-8 的主题／正文为“课程通知”／“请查看课程安排”，Base64、quoted-printable 和 GB18030 覆盖中文；alternative 同时含两种正文时选择 plain；HTML 提取“面试 & 报到”并忽略 script；mixed 验证附件“简历.pdf”、类型和长度；缺主题、空原文和不支持字符集返回明确结果。
- [ ] 运行 `mime`，确认有效中文样本在实现前失败。
- [ ] 用 GMime 解析邮件头、传输编码和字符集，检查输出 UTF-8；未知字符集明确失败，不输出乱码。
- [ ] 用 libxml2 提取 HTML 文本，忽略 script/style，不请求外部资源；附件不作为正文，保留元数据和原始邮件。
- [ ] 实施限制：原文最大 20 MiB、MIME 嵌套最大 32 层、部件最大 256 个；超限返回明确 `parse_error`。
- [ ] 运行样本、三个边界回归和全套 CTest；通过后在依赖记录中固定实际 GMime／libxml2 版本。

**Acceptance:**

- 固定样本的主题、正文、附件元数据和不支持字符集行为与断言一致。
- 20 MiB+1、嵌套 33 层、257 个部件均被明确拒绝，其他邮件仍可解析。
- `mime` 和全套 CTest 通过，不能只依据库文档宣称通过。
- 只暂存本任务文件，提交 `feat: parse Chinese MIME mail`。

## Task 4: 只读 IMAP 接入

**Files:** `include/mailai/imap.h`、`src/mail/imap.cpp`、`tests/imap_test.cpp`、`tests/imap_server.py`。

**Interfaces:** 消费 `ImapSettings` 与密码值，产出 `ImapSource`；`snapshot` 返回 UIDVALIDITY 和 UID 列表，`fetch` 返回匹配 `RemoteKey` 的原文和 INTERNALDATE。

**Steps:**

- [ ] 建立本机可控 TLS IMAP 测试服务器，由测试脚本在临时目录生成证书；C++ 测试调用实际 libcurl，覆盖正常读取、空箱、无效凭据、不受信任证书、超时、临时断连重试、错误 UID／UIDVALIDITY、抓取前邮件消失和 INTERNALDATE。
- [ ] 运行 `imap`，确认正常收取在实现前失败；默认 CTest 只连接临时 localhost，不连接学校邮箱。
- [ ] 实现 libcurl 适配：会话 RAII；URL、用户名和密码分别设置；使用 `BODY.PEEK[]`；元数据与邮件字节分开处理，不从正文搜索协议标识；错误不得包含测试密码。
- [ ] 实现超时和重试：临时网络错误最多重试 2 次，认证失败不重试；重连后重新核对 UIDVALIDITY，范围变化时拒绝把旧键绑定到新正文；正文超过 20 MiB 时停止并返回可重试错误。
- [ ] 验证初始未读邮件读取后仍未读，并检查错误类型、耗时边界和诊断脱敏。

**Acceptance:**

- 正常、空箱、认证、证书、超时、重试、远端消失和日期场景均有实际网络断言。
- 抓取使用服务器 UID／UIDVALIDITY 定位，不使用列表序号；服务器已读状态不变。
- `imap` 和全套 CTest 通过；只建立 TCP 连接不算通过。
- 只暂存本任务文件，提交 `feat: fetch mail through IMAP`。

## Task 5: 增量同步、去重与恢复

**Files:** `include/mailai/sync.h`、`src/app/sync.cpp`、`tests/sync_test.cpp`。

**Interfaces:** 消费实际 `SqliteStore`、MIME 和可替换的 `ImapSource`，产出 `sync_account`；此函数不打印，资源装配由 Task 6 负责。

**Steps:**

- [ ] 编写编排测试：首次 UID 1..30 只安排 11..30；UID 10 失败、11 成功并重启后只重试 10 并收取 12，最终三封且 11 的 ID 不变；重复同步成功数为 0 且行数不变；发现队列已提交但尚未抓取时重启可继续；解析失败不阻断其他邮件；远端消失不无限重试；UIDVALIDITY 100→200 保留历史。
- [ ] 运行 `sync`，确认恢复或成功入库行为尚未实现时失败；网络边界用按 `RemoteKey` 返回固定字节／错误的测试源，SQLite 和 MIME 使用真实实现。
- [ ] 实现发现→入队→抓取／解析→逐项事务保存；快照先去重并按 UID 排序；首次只排最近 20 个。
- [ ] 发现游标只与持久待办一起提交，不用“最大成功 UID”代替；成功项不再下载，临时抓取失败保留任务，解析失败保存 raw 和 failed 状态，下轮可重试。
- [ ] 当前完整快照无该 UID 或抓取明确返回 `remote_gone` 时记录失败并停用待办，保留本地历史；持久化整体失败立即终止，禁止继续推进进度。

**Acceptance:**

- 首次范围、重复同步、失败恢复、重启继续、坏邮件隔离和 UIDVALIDITY 变化的结果与固定期望一致。
- 空箱／无新邮件 `success=0,failed=0`；部分失败包含逐 UID 原因；最后成功时间只在本轮无失败时更新。
- `sync` 和全套 CTest 通过；所有恢复测试使用真实 SQLite。
- 只暂存本任务文件，提交 `feat: resume incremental mail sync`。

## Task 6: 真实缓存与 CLI 接入

**Files:** `src/cli.h`、`src/cli.cpp`、`src/runtime.h`、`src/runtime.cpp`、`src/main.cpp`、`tests/cli_test.cpp`、`tests/runtime_test.cpp`、`tests/cli_smoke.cmake`、`CMakeLists.txt`、`README.md`。

**Interfaces:** CLI 只消费所需服务，不再依赖整箱 `vector<Mail>`；`inbox` 读取列表投影，`show` 才读取正文。保留核心内存查询供 demo 使用。

```cpp
enum class DataOrigin { demo, local_cache };

struct CliServices {
    DataOrigin origin;
    std::function<Result<std::vector<InboxItem>>(const InboxQuery&)> list_inbox;
    std::function<Result<Mail>(MailId)> find_mail;
    std::function<Result<SyncSummary>(const AccountId&)> sync;
};

int run_cli(const std::vector<std::string>& args, const CliServices& services,
            std::ostream& out, std::ostream& err);

using SecretLookup = std::function<std::optional<std::string>(std::string_view)>;
CliServices make_demo_services();
CliServices make_local_services(Environment env, SecretLookup read_secret);
```

服务集合及回调拥有捕获值；创建服务时不打开文件或网络。

**Steps:**

- [ ] 保留现有 31 次 CLI 调用并适配新入口，增加缓存查询、无缓存、demo、参数错误、同步成功／失败／部分失败和退出码测试；验证 `UINT64_MAX` 查询返回 1、溢出返回 2。
- [ ] 运行 `cli`、`runtime` 和全套测试，观察当前入口不支持服务装配、新命令或来源显示的失败；正常参数完成校验前不得访问存储或网络。
- [ ] 实现真实服务的按需装配：读命令只读 SQLite，不读取密码；sync 校验配置后获取写锁、打开存储、连接网络并调用同步服务。
- [ ] 实现显示：inbox 只读列表投影，show 读取正文和附件；明确显示解析失败与历史状态；终端输出处理 ESC、换行和制表符，正文保留正常换行；原文和缓存保持原值。
- [ ] 更新帮助和 README 的环境变量、来源模式与 0/1/2/3 退出码；接入 `config`、`store`、`mime`、`imap`、`sync`、`runtime`，最终默认 CTest 共 9 项。
- [ ] 运行 cli_smoke 的 `--demo inbox`，确认不依赖真实账号；验证缺配置的 help 和离线查询不触发收信。

**Acceptance:**

- 无密码且网络不可用时缓存查询成功；无缓存显示 0 封且不混入样例。
- `--demo` 行为保留；`--demo sync`、缺失／重复账号和未知参数返回 2 且不调用服务。
- 同步全成功／全失败／部分成功分别返回 0／1／3，并显示计数和逐 UID 原因。
- 业务错误的 `stage` 随诊断显示；终端不输出原始 ESC；中文完整；9 项 CTest 全部通过。
- 只暂存本任务文件，提交 `feat: connect CLI to persistent mail cache`。

## Task 7: 真实邮箱验收与阶段交付

**Files:** `tests/live_mailbox_test.cpp`、`README.md`、`docs/dependencies.md` 和本计划的状态行；必要修复按失败用例先行。

**Steps:**

- [ ] 用户在本机 WSL 按学校说明生成客户端专用密码并设置环境变量；真实验收目录使用项目 `build/live-data` 或其他不入 Git 的路径。
- [ ] 维护独立 `live_mailbox_test` 目标，复用真实存储、网络和同步入口；只在 `MAILAI_LIVE_TESTS=1` 时运行，不注册默认 CTest，使用独立 `build/live-probe` 目录和 `SyncOptions{5}`。
- [ ] 执行 sync → inbox → show → 重启 → 再次 inbox／show → 重复 sync，确认可读中文、稳定 ID、无重复行；移除本机密码后离线查看仍可用。
- [ ] 在邮箱界面复核读取前后已读状态；端点或认证失败时记录实际错误，不把离线测试当作真实接入成功。
- [ ] 在 Debug 和新的 `build/verify-stage2-release` 目录运行全部 9 项 CTest，检查无警告、无默认真实网络依赖、原有功能不回归。
- [ ] 独立只读审阅整个阶段；有重要问题先写失败回归再修复；更新依赖与验收记录并限定路径提交 `docs: record single mailbox acceptance`。

**Acceptance:**

- 真实邮箱可收取并离线查看首次范围内的最近邮件（最多 20 封），中文、附件元数据、失败状态和历史状态可辨认。
- 重复同步不产生重复行，稳定 ID 不变；失败项恢复、部分成功和 UIDVALIDITY 变化均有可追溯结果。
- 默认 9 项 CTest 在 Debug／Release 均通过，真实收信探针只在显式启用时运行。
- 记录不包含密码或真实正文；外部配置阻塞时明确说明阶段 2 尚未完成。

## Verification

每个任务只接入自己的目标；执行以下命令，预期配置、构建和全部已接入 CTest 均退出 0，无编译警告。测试失败时先定位原因，不跳过失败项。

```powershell
wsl.exe -d Ubuntu -- cmake -S /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant -B /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
wsl.exe -d Ubuntu -- cmake --build /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug
wsl.exe -d Ubuntu -- ctest --test-dir /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug --output-on-failure
```

阶段结束时使用新的 `build/verify-stage2-release` 目录，将构建类型替换为 `Release` 后重复以上步骤。若目录已存在，使用新的无日期名称并保留已有产物。

## References

- [RFC 9051](https://www.rfc-editor.org/rfc/rfc9051.html)：UID／UIDVALIDITY 和 `BODY.PEEK` 语义。
- [libcurl IMAP 示例](https://curl.se/libcurl/c/imap-fetch.html)：协议适配参考；不替代本项目验证。
- [SQLite 数据类型](https://www.sqlite.org/datatype3.html)：SQLite INTEGER 的有符号范围。
- [GMime](https://github.com/jstedfast/gmime)、[libxml2 HTML API](https://gnome.pages.gitlab.gnome.org/libxml2/html/HTMLparser_8h.html)：MIME、字符集和 HTML 文本提取依据。
- [北交大客户端专用密码教程](https://ic.bjtu.edu.cn/fwzn/xxxtzn/202311421.htm)：本人客户端凭据来源；服务器参数仍以本人设置页为准。

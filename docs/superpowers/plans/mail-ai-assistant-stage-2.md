# Mail AI Assistant Stage 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 接入一个真实学校邮箱，将收件箱邮件解析并保存到 SQLite，支持离线查看、重复同步去重和中断后继续。

**Architecture:** 保留 CLI → 业务服务 → 邮件／存储的依赖方向。同步服务串行调用 IMAP、MIME 和 SQLite；查询命令只读取本地缓存。用持久待处理队列记录已发现但尚未完成的 UID，防止失败邮件被同步进度越过。

**Tech Stack:** C++23、CMake 3.20+、Ninja、CTest、WSL Ubuntu；拟采用 libcurl 8.0+、SQLite 3.37+、GMime 3.2+ 和 libxml2 2.9+。Python 3 与 OpenSSL 命令行仅用于本地 TLS 测试服务器和测试证书。

**Spec:** [已批准的整体设计](../specs/mail-ai-assistant-design.md)，本计划细化第 3、7、8 节与第 9 节阶段 2。[阶段 1](mail-ai-assistant-stage-1.md) 已完成，代码基线为 `f4948ab`。

**Status:** Task 1 进行中，Step 1、2 已完成。用户已纠正 parse_message 拼写；sync_types.h、mime.h 已通过独立翻译单元的 C++23 语法检查，解析函数声明调用也通过编译检查。WSL Debug 构建无警告，现有 4 项 CTest 全部通过，包括 XDG 修复与排序回归。下一项按带写节奏逐库接入依赖，先让用户思考 SQLite 开发依赖与 CMake 接入方案；当前 pkg-config 未找到 sqlite3。其余依赖及 Task 1 完整验收尚待完成；解析函数实现属于 Task 3。

## Global Constraints

- 新文件按主题命名，不加日期前缀；专注本项目，暂停原实习计划和 CCG 记录。
- 指导顺序按用户最新要求：导师先说明下一项任务的目标、输入输出与约束；用户自行思考并讲述方案；导师点评后再提供必要提示或代码并带着实现，最后测试验证。每轮聚焦一个小任务，等待用户讲思路期间不提前给出实现；测试默认由导师维护，用户可参与。明确委托直接实施的项目按授权执行。
- 语言标准采用 C++23；核心库公开 `cxx_std_23`，所有项目目标设置 `CXX_STANDARD 23`、`CXX_STANDARD_REQUIRED ON`、`CXX_EXTENSIONS OFF`。
- 一个可执行程序 `mail_assistant` 调用一个核心库 `mailai_core`；持久记录拥有自己的字符串和容器，句柄通过 RAII 管理。
- 构建、测试和运行通过 `wsl.exe -d Ubuntu -- ...` 调用 WSL 工具。
- 首个接入对象沿用用户最早选择的学校邮箱，默认账号 ID 为 `school`。主机、端口和用户名按本人设置页填写，不硬编码尚未实测的学校服务器参数。
- 凭据值放在本地环境变量，数据库只保存凭据引用；日志排除凭据。接入时使用学校客户端专用密码，用户在本机配置，无需发到对话中。
- 本阶段收取 `INBOX`，首次取最高 UID 对应的最近 20 封；后续获取新邮件和仍待重试的邮件。使用 `BODY.PEEK[]`，保持服务器已读状态。
- 网络操作默认连接超时 10 秒、总超时 30 秒；临时连接错误最多重试 2 次，退避 1、2 秒；认证、证书校验、参数和解析错误直接报告。
- 正常结果写 stdout，进度和诊断写 stderr。0 全部成功，1 全部失败或无法执行，2 参数错误，3 部分成功；空邮箱和无新邮件返回 0。
- 本阶段不实现 AI、SMTP、回复、多账号管理、watch 和 JSON 输出；这些功能沿用后续阶段安排。

## Review Focus

1. 较小 UID 失败、较大 UID 成功后重启：失败项仍会重试，成功项不重复。Task 2、5 覆盖。
2. UIDVALIDITY 改变，或抓取期间服务器标识范围变化：历史保留并标记，旧范围任务不用于新邮件。Task 2、4、5 覆盖。
3. `uint64_t` 邮件 ID 超过 SQLite 有符号范围：查询返回未找到，禁止截断或变成负数。Task 2、6 覆盖。
4. 中文、HTML、multipart、附件和损坏邮件混合：可解析正文正确，失败项保留原文与索引，不中断其他项。Task 3、5 覆盖。
5. 未配置凭据时离线查看，以及无效参数、重复 sync、数据库写失败：查询不连接网络，参数错误先返回，失败不会破坏缓存。Task 1、2、6 覆盖。

---

## 本阶段的细化决定

| 事项 | 决定 |
|---|---|
| 数据来源 | 默认读取真实本地缓存；阶段 1 样例通过显式 `--demo` 使用 |
| MIME 候选调整 | 优先验证 GMime；它负责 MIME 与字符集转换，libxml2 负责 HTML 转文本 |
| 原 mailio 候选 | 暂不采用，当前上游存在未关闭的 Q 编码地址解析问题报告；不将报告等同于本项目已复现漏洞 |
| 依赖获取 | 使用 WSL 系统开发包；离线样本通过后在依赖记录中写明实际版本，不追踪浮动 master |
| 首次同步 | 注册最近 20 个 UID；更早邮件本阶段不回填 |
| 增量方式 | 本阶段扫描 UID 列表，在本地筛选新 UID，只下载新项与待重试项；先保证正确性 |
| 正文取舍 | multipart/alternative 优先 text/plain，缺失时将 text/html 转文本；附件只保存名称、类型、大小 |
| 删除与标记 | 不删除远端邮件，不修改远端已读状态；本地历史不因远端消失而自动删除 |

已只读检查环境：`pkg-config` 未找到 `libcurl`、`sqlite3`、`gmime-3.0`、`libxml-2.0`；依赖安装与集成验证属于 Task 1，不视为已经可用。

## 命令与配置

**评审跟进（用户已要求按带写方式落实）：** 保持现有架构与任务顺序，测试和文档由导师维护；业务实现先由用户讲述思路，导师评审后带写。

- [x] Task 1：忽略非绝对路径的 XDG_DATA_HOME，有 HOME 时回退，无其他来源时报 config_error。已观察运行 RED，用户写入 is_absolute 判断后，在本机 WSL 运行给定构建／测试命令并反馈全部通过。
- [x] Task 1：加强内存排序测试，使用 id=7/time=50 与原有 42/43/44，对全部 24 种输入排列断言固定期望 42/43/44/7；用户本机反馈测试全部通过。Task 2 的 SQLite 查询沿用相同排序契约。
- [ ] Task 2：在 SqliteStore::complete 统一检查远端定位并形成持久邮件状态。真实远端键非空且 UIDVALIDITY／UID 非零；解析成功清空解析错误，解析失败保留原文、非空诊断与待重试项，并采用计划规定的失败主题和空正文。无效输入不修改已有记录或待办；样例 Mail 仍允许没有远端定位。通过真实临时 SQLite 用例验证，不新增未被业务消费的校验层。
- [ ] Task 6：按既有 CliServices 接口解开 CLI 与整箱 vector<Mail> 的依赖；inbox 仅读取列表投影，show 才读取正文，业务错误的 stage 随诊断显示。沿用既有 CLI 回归并验证只调用所需服务。
- [x] 文档：总体规格中的 MIME 候选统一指向本计划当前的 GMime／libxml2 选择，历史选型留在调研记录。

| 命令 | 行为 |
|---|---|
| `mail_assistant --help` | 显示帮助，不打开数据库或连接网络 |
| `mail_assistant sync --account school` | 同步配置账号，输出成功／失败计数，失败定位到 UID |
| `mail_assistant inbox [--account school]` | 只读本地列表，显示“数据来源：本地缓存” |
| `mail_assistant show <id>` | 只读本地详情，显示附件元数据及解析／历史状态 |
| `mail_assistant --demo inbox` | 阶段 1 固定样例，显示“数据来源：示例” |
| `mail_assistant --demo show 43` | 查看固定样例详情 |

`--demo` 只允许出现在开头；`--demo sync` 返回参数错误 2。无参数仍显示帮助。`inbox --account all` 保留。

配置先使用环境变量，避免为首个账号增加配置文件解析依赖：

| 变量 | 规则 |
|---|---|
| `MAILAI_DATA_DIR` | 可选；缺省为 `$XDG_DATA_HOME/mail-ai-assistant`，再缺省为 `$HOME/.local/share/mail-ai-assistant`；XDG_DATA_HOME 为相对路径时忽略该来源 |
| `MAILAI_ACCOUNT_ID`、`MAILAI_ACCOUNT_NAME` | 可选，缺省 `school`、`学校邮箱` |
| `MAILAI_EMAIL`、`MAILAI_IMAP_HOST`、`MAILAI_IMAP_USER` | sync 必填，拒绝空值 |
| `MAILAI_IMAP_TLS` | `implicit`（默认）或 `starttls`，均要求证书和主机名验证 |
| `MAILAI_IMAP_PORT` | 可选；implicit 缺省 993，starttls 缺省 143；十进制 1–65535 |
| `MAILAI_PASSWORD_ENV` | 可选，缺省 `MAILAI_IMAP_PASSWORD`；配置和数据库只保存该名称 |
| 上一项指定的环境变量 | 用户本机设置客户端专用密码，仅创建连接时读取 |

`inbox`、`show` 只需要数据目录，不要求 IMAP 配置或密码。尚无数据库时，全部收件箱返回 0 封，指定未知账号／邮件按既有错误规则处理；不自动展示样例。

## 文件结构与接口约定

以下路径相对于项目根目录。新增业务文件按带写方式创建；测试由用户参与编写，导师提供参考、审查并维护，固定虚构样本由导师维护。

| 文件 | 责任 | Task |
|---|---|---|
| `include/mailai/model.h`、`include/mailai/inbox.h`、`src/app/inbox.cpp` | 兼容扩展解析／历史状态与附件信息，投影保留状态 | 1 |
| `include/mailai/config.h`、`src/config.cpp` | 配置、数据目录与连接参数校验 | 1 |
| `include/mailai/sync_types.h`、`include/mailai/mime.h` | 服务器定位、同步与解析值类型 | 1 |
| `include/mailai/store.h`、`src/store/sqlite_store.cpp` | SQLite、schema、查询、事务与待处理项 | 2 |
| `include/mailai/data_dir_lock.h`、`src/store/data_dir_lock.cpp` | 单数据目录写进程锁 | 2 |
| `src/mail/mime.cpp`、`src/mail/html_text.h`、`src/mail/html_text.cpp` | MIME、字符集和 HTML 正文提取 | 3 |
| `include/mailai/imap.h`、`src/mail/imap.cpp` | libcurl IMAP 适配、TLS、超时与重试 | 4 |
| `include/mailai/sync.h`、`src/app/sync.cpp` | 持久队列和单账号同步编排 | 5 |
| `src/cli.h`、`src/cli.cpp` | CLI 服务入口、sync 展示、来源与状态显示 | 6 |
| `src/runtime.h`、`src/runtime.cpp`、`src/main.cpp` | 装配真实／样例服务，按需打开资源 | 6 |
| `tests/config_test.cpp`、`tests/store_test.cpp` | 配置与真实临时 SQLite 测试 | 1、2 |
| `tests/mime_test.cpp`、`tests/fixtures/mail/*.eml` | 虚构 MIME 样本与期望值 | 3 |
| `tests/imap_test.cpp`、`tests/imap_server.py` | 实际 libcurl 对接本机可控 TLS 服务器 | 4 |
| `tests/sync_test.cpp` | 可控收信结果＋真实 SQLite 验证恢复 | 5 |
| `tests/cli_test.cpp`、`tests/runtime_test.cpp`、`tests/cli_smoke.cmake` | 保留既有用例，增加缓存与 sync 验证 | 6 |
| `tests/live_mailbox_test.cpp` | 显式运行的真实收信探针，不注册进默认 CTest | 7 |
| `CMakeLists.txt`、`README.md`、`docs/dependencies.md` | 依赖、构建、命令与实际验证记录 | 各任务随交付更新 |

数据类型的字段约定：

| 类型 | 字段 |
|---|---|
| `AttachmentInfo` | `string name, media_type; uint64_t size_bytes{}` |
| `ParseState` | `parsed, failed` |
| `Mail` 增量字段 | `string folder{}`、`uint32_t uid_validity{}, uid{}`、`ParseState parse_state{ParseState::parsed}`、`parse_error`、`historical{false}`、`message_id`、`in_reply_to`、`references`、`vector<AttachmentInfo> attachments` |
| `InboxItem` 增量字段 | `ParseState parse_state{ParseState::parsed}`、`historical{false}` |
| `TlsMode` | `implicit, starttls` |
| `ImapSettings` | `string host, username, credential_env`；`uint16_t port{993}`；`TlsMode tls_mode{TlsMode::implicit}`；`chrono::milliseconds connect_timeout{10000}, total_timeout{30000}`；`optional<filesystem::path> ca_file` |
| `RuntimeConfig` | `Account account; ImapSettings imap; filesystem::path data_dir`，不含密码值 |
| `RemoteKey` | `AccountId account_id; string folder; uint32_t uid_validity{}, uid{}`；两项服务器数值必须非零 |
| `MailboxSnapshot` | `uint32_t uid_validity{}; vector<uint32_t> uids`，本阶段固定 INBOX |
| `FetchedMail` | `RemoteKey key; int64_t received_at_utc{}; string raw`，时间取服务器 INTERNALDATE |
| `ParsedMail` | `sender, subject, body, message_id, in_reply_to, references` 字符串与 `attachments` |
| `SyncState` | `optional<uint32_t> uid_validity`、`uint32_t last_discovered_uid{}`、`optional<int64_t> last_attempt_at, last_success_at` |
| `SyncFailure` | `RemoteKey key; Error error` |
| `SyncSummary` | `size_t discovered{}, success{}, failed{}; bool reindexed{}; vector<SyncFailure> failures` |

所有新增成员提供默认值，保留原字段顺序，使阶段 1 的聚合初始化仍可用。`Mail.id` 默认初始化为 0。`ErrorCode` 保留原项，增加 `config_error, auth_error, network_error, protocol_error, remote_gone, parse_error, storage_error, busy`；`Error` 末尾增加默认空的 `stage`，用于显示失败阶段。

references 暂存展开后的原始头字段字符串，原始完整邮件始终保留；会话归并留到后续阶段。服务器时间不能用邮件 Date 头替代。解析失败行使用主题“(解析失败)”、空正文及明确错误；成功解析但缺主题时使用“(无主题)”，缺可选 Message-ID／回复头允许空值。

共享接口如下；类型先由 Task 1 建立，具体实现由所属任务完成：

```cpp
using Environment = std::map<std::string, std::string>;
template <class T>
using Result = std::expected<T, Error>;
using Status = Result<void>;

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
    // RAII，禁止拷贝，允许移动。
};

class DataDirLock {
public:
    static Result<DataDirLock> acquire(const std::filesystem::path&,
                                      std::chrono::milliseconds wait);
    // RAII，禁止拷贝，允许移动。
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

`make_imap_source` 返回的回调必须拥有所需会话状态，不捕获已销毁的局部变量。测试用固定结果替换该网络边界；存储和 MIME 解析使用真实实现。

## Task 1：配置、值类型与依赖基础

**Files:** 上表 Task 1 文件，`tests/config_test.cpp`、`CMakeLists.txt`、`docs/dependencies.md`。

**Interfaces:** 消费阶段 1 的 Account、Mail、Error、Result；产出上述新增值类型、`resolve_data_dir` 与 `parse_sync_config`。

**带写进度与拆分：** 数据目录优先级测试已写入，包含显式目录、XDG、HOME 三个场景；config.h 声明与 config.cpp 返回空路径的最小空壳也已由用户写入，并经导师静态核对。用户先编写 CMake 接入，本轮再明确委托导师重写 CMakeLists.txt：统一公共标准与警告配置，将四个可执行目标链接到 mailai_core，修正 mail_core 拼写，保留现有 CLI 与测试。业务源码仍由用户带写。为适应用户要求的逐步带写，按单一行为分别进行测试→声明／空壳→运行失败→实现→验证的小循环；其余配置测试、共享类型、依赖和本任务完整验收仍须逐项完成，上述 Task 1 不能提前标为完成。

**数据目录 RED 证据：** 通过 WSL 执行 `cmake -S <项目根目录> -B <项目根目录>/build/debug -DCMAKE_BUILD_TYPE=Debug` 与 `cmake --build <项目根目录>/build/debug`，均退出 0，无编译警告。`ctest --test-dir <项目根目录>/build/debug --output-on-failure` 退出 8：inbox、cli、cli_smoke 通过，config 在 `explicit data directory should take precedence` 断言抛出 runtime_error 后失败；当前空路径返回已被测试发现。尚未实现正确解析，不能将全套测试记为通过。

**缺失配置 RED 证据：** 导师补充 test_missing_data_dir_sources，验证空 Environment 应返回 Error，并先于优先级测试运行。WSL 中重新构建 config_test 退出 0；运行 `ctest --test-dir <项目根目录>/build/debug -R '^config$' --output-on-failure` 退出 8，实际断言为 `missing data directory sources should return an error`。随后给用户 ErrorCode 增加 config_error 和 resolve_data_dir 实现的参考，用户已写入，运行结果见下方 GREEN 证据。

**数据目录 GREEN 证据：** 用户实现按专用目录→XDG→HOME 选择非空配置，均不可用时返回 config_error。导师通过 WSL 执行 `cmake --build <项目根目录>/build/debug` 和 `ctest --test-dir <项目根目录>/build/debug --output-on-failure`，均退出 0，编译无警告，inbox、cli、config、cli_smoke 共 4/4 通过。当前 config 包含优先级和完全缺失两项测试函数；空值回退及具体错误码断言待用户补充，不将其记为已经验证，也不将 Task 1 整体标为完成。

**边界测试复核：** 用户在 test_data_dir_precedence 末尾删除 HOME 并断言 config_error；此时三个键都已删除，验证的是完全缺失配置。WSL 构建和全套 CTest 均退出 0，共 4/4 通过，无编译警告。允许用户合并在原测试函数中，但空字符串回退仍未覆盖；下一步在函数末尾重新准备包含空值的 Environment，逐项验证空专用目录→XDG、空 XDG→HOME、全部空值→config_error，不将 erase 等同于空字符串输入。

**空值边界补齐：** 用户明确委托导师直接补好此部分，并要求简洁说明。导师在原 test_data_dir_precedence 中追加空专用目录回退 XDG、空 XDG 回退 HOME、全部空值返回 config_error 三组断言，保留用户原有缺失配置用例，统一英文提示并修正 date/data 笔误。生产源码未修改；此处属于现有行为的补充回归覆盖，不宣称新增断言经历过 RED。通过 WSL 重新构建并运行全部 CTest，均退出 0，无编译警告，inbox、cli、config、cli_smoke 共 4/4 通过。

**收信配置带写进度：** 导师已补 test_valid_sync_config，用虚构主机、地址、用户名和数据目录验证配置字段传递，以及默认 school／学校邮箱、implicit／993、凭据引用 MAILAI_IMAP_PASSWORD；测试输入不包含密码值。当前提供 config.h 所需三种类型与 parse_sync_config 声明，以及 config.cpp 返回“尚未实现”错误的可链接空壳参考，待用户写入后构建并观察 `valid sync config should be accepted` 断言失败。未运行缺声明的测试，不将编译／链接缺失当成运行 RED；CMake 已编译 config.cpp，本步无需更改 CMake。

**收信配置 RED 证据与测试补齐：** 用户已写入三种类型、声明和返回 config_error 的空壳。WSL 全量构建退出 0，无警告；全套 CTest 退出 8，inbox、cli、cli_smoke 通过，config 在 `valid sync config should be accepted` 断言失败。导师随后增加 test_starttls_default_port、test_custom_sync_config、test_explicit_port_boundaries、test_invalid_config；覆盖 143 默认端口、显式 1143、自定义账号与凭据引用、1/65535 边界及 16 组非法／缺失输入。新增测试编译通过，单独运行 config 仍在有效配置断言处退出 8；因该断言先失败，不声称后续用例已经执行。下一步由用户按参考添加文件内辅助函数，并实现必填项、默认值、目录复用、TLS 模式与严格端口解析；当前业务源码仍是空壳。

**配置细化：** 可选配置未设置或为空字符串时采用默认值；必填字段未设置或为空均返回 config_error。端口先按较宽的无符号整数解析，确认完整消费十进制字符串且落在 1–65535 后才转换为 uint16_t；禁止先窄化再校验。凭据只保存环境变量名称，配置解析不读取密码值或连接网络。

**收信配置 GREEN 证据：** 用户已写入 config_value、make_config_error 与 parse_sync_config，端口解析使用结构化绑定取得 from_chars 的 ptr/ec，其余行为符合上述参考。导师通过 WSL 执行 `cmake --build <项目根目录>/build/debug` 和 `ctest --test-dir <项目根目录>/build/debug --output-on-failure`，均退出 0，无编译警告，4/4 通过。config 的 7 个测试函数均已执行，包括 16 组非法输入、STARTTLS 143、显式覆盖、1/65535 端口边界和数据目录回退。当前属于纯配置解析验证，未连接真实邮箱，未完成 Task 1 的剩余共享类型与第三方依赖集成。

**模型状态带写进度：** 导师补充 test_inbox_preserves_message_state：以现有虚构邮件的副本构造当前解析失败、历史解析成功、历史解析失败三种组合，检查排序后的稳定 ID 42/43/44 与各自 parse_state、historical 一致，防止字段遗漏或两种状态被错误关联。测试与 main 调用已写入；业务头文件和 list_inbox 尚未修改。当前提供 ParseState 与两个默认成员的参考，待用户写入后运行；缺字段／缺类型的编译诊断不记作运行 RED。为保留实际失败证据，列表中复制状态的实现留到观察运行断言失败之后带写，CMake 无需变更。

**模型状态 RED 证据：** 用户已定义 ParseState，并在 Mail、InboxItem 末尾添加默认 parsed／false 的两个成员。通过 WSL 全量构建退出 0，无警告；全套 CTest 退出 8，inbox 在 `inbox should preserve a failed parse state` 断言失败，cli、config、cli_smoke 通过。原因已由代码和结果对应确认：list_inbox 的 InboxItem 聚合初始化仅填原有六个字段，新增字段采用默认值，未传递源邮件状态。当前给用户添加 mail.parse_state、mail.historical 的参考，业务转换实现尚待用户写入和验证。

**模型状态 GREEN 证据：** 用户已在 InboxItem 初始化末尾复制 mail.parse_state、mail.historical。导师通过 WSL 全量构建和运行全套 CTest，均退出 0，无编译警告，inbox、cli、config、cli_smoke 共 4/4 通过；inbox 的 10 个测试函数均执行，包括状态在排序后仍与正确邮件 ID 关联。此项完成测试失败→实现→通过的循环。

**邮件信息字段带写进度：** 当前给用户 AttachmentInfo 和完整 Mail 值类型参考，补充文件夹、UIDVALIDITY／UID、解析错误、回复关联头及附件元数据，并按计划将 Mail.id 默认初始化为 0。保留原六个字段的顺序，新增字段均提供默认值，列表项中已验证的两个状态及其复制逻辑保持适用。当前仅补数据声明，不引入新的解析／存储行为，不为字段存在性或默认构造重复编写测试；实际字段填充与持久化在 MIME、IMAP、存储任务验证。业务头文件尚待用户写入，下一轮检查构建兼容性与全套测试。

**邮件信息字段兼容性验证：** 用户已写入完整 Mail 与 AttachmentInfo。导师通过 WSL 执行 Debug 全量构建和全套 CTest，均退出 0，无编译警告，inbox、cli、config、cli_smoke 共 4/4 通过。此结果验证新增声明与现有功能兼容，不代表尚未实现的 MIME、IMAP 或持久化行为已验证。下一步给用户 ErrorCode 扩展、默认空的 Error.stage 和 Status 别名参考，待写入后复核；同步共享类型与依赖集成仍未完成，Task 1 保持进行中。

**Task 1: Ruling：错误结果类型迁移。** 用户明确选择并委托统一改用 C++23 std::expected；Result<T> 从 variant<T, Error> 改为 expected<T, Error>，Status 改为 Result<void>。同步更新查询、配置、CLI 和现有测试，读取错误前检查失败状态；保留字段、错误上下文、排序、输出和退出码。顺手修正尚未使用的 protocal_error 拼写为 protocol_error。此项覆盖原阶段 1 的错误容器选择；旧计划中的 variant 示例保留为历史记录。代价是标准库必须支持 C++23 expected，且外部调用方需一起迁移。

**expected 迁移验证：** 迁移前通过 WSL 全量构建及 CTest，4/4 通过；先调整既有业务测试的结果访问方式，构建因旧 variant 缺少 value／has_value／error 接口而退出 1，此为编译期接口检查，不记作运行断言 RED。随后迁移生产接口及所有调用方，Debug 全量构建与全套 CTest 均退出 0，无警告，inbox、cli、config、cli_smoke 共 4/4 通过。测试仍验证真实配置与查询行为，不新增仅验证标准库或类型别名的测试；Task 1 的同步共享类型、依赖集成与整体验收仍待完成。

**XDG 相对路径 RED：** 导师新增 test_relative_xdg_data_home_is_ignored：相对 XDG_DATA_HOME 与绝对 HOME 同时存在时应回退 HOME，移除 HOME 后应返回 config_error。通过 WSL 构建退出 0、无警告；全套 CTest 退出 8，config 在 `relative XDG_DATA_HOME should be ignored in favor of HOME` 断言失败，其余 3 项通过。根因是 config.cpp 的 XDG 分支只检查非空并直接返回拼接结果；移除 HOME 后的后半组断言尚未执行。当前提供 path::is_absolute 判断的参考，生产修复待用户写入；不记为已修复。

**评审改进待验证：** 用户已在 XDG 分支构造 filesystem::path 并只在 is_absolute() 时返回，静态核对与参考一致。导师新增 test_inbox_order_is_independent_of_input，用固定期望 42/43/44/7 检查四封邮件的 24 种输入排列；其中 id=7 的邮件时间为 50，避免只按 ID 排序也通过。该测试尚未编译或运行，不声称已验证全部排列或测试已通过。首次 WSL 构建／全套测试请求在执行前因自动审批超时被拒绝，按工具提示重试一次仍超时；普通沙箱尝试同一 WSL 命令退出 1，报 UtilBindVsockAnyPort socket failed。没有再次升级重试，没有改用宿主环境执行项目代码。待用户本机 WSL 验证或运行权限恢复后，先运行完整构建与全套 CTest；上述两项勾选保持未完成。

**评审改进本机验收：** 用户随后反馈按给定 WSL 构建／全套 CTest 命令运行，测试全部通过，据此完成上方 XDG 与排序两项。验证来源为用户本机反馈，导师未取得完整输出，不追加具体退出码、警告或耗时结论。下一步提供 RemoteKey、MailboxSnapshot、FetchedMail 的头文件参考，供用户写入，纯值类型的有效性由后续收信与存储入口及其业务测试验证。

**共享类型与接口验证：** 用户已完成六个同步值类型、ParsedMail 和 parse_message 声明。导师在 build/debug 放置临时编译探针：分别单独包含 sync_types.h、mime.h，并在后者中检查按既定名称调用解析接口可返回 Result<ParsedMail>。通过 WSL 使用 c++ -std=c++23 -Wall -Wextra -Wpedantic -fsyntax-only 检查两个翻译单元，再构建 Debug 并运行全套 CTest，串联命令退出 0，无警告，4/4 通过。此检查验证声明及现有功能，不链接或执行尚未实现的解析函数。随后只读执行 pkg-config --modversion sqlite3，退出 1，提示未找到 sqlite3.pc；尚未安装依赖或修改其 CMake 配置。先以 SQLite 完成一轮依赖带写，再处理其余库，Step 3／4 保持未完成。

- [x] **Step 1：带写配置测试，用户参与。** `test_valid_sync_config` 断言默认 school、端口 993、凭据引用 MAILAI_IMAP_PASSWORD；`test_starttls_default_port` 断言 143；`test_data_dir_precedence` 依次检查显式目录、XDG、HOME；`test_invalid_config` 覆盖空主机、缺用户名、未知 TLS、端口 0/65536/-1/993x；在 inbox 测试加入 `test_inbox_preserves_message_state`，断言列表保留邮件的 failed／historical 状态。输入均为虚构数据，不修改真实环境变量。
- [x] **Step 2：先建立声明与最小可链接空壳，运行 config 测试。** 有效配置用例先断言结果为 RuntimeConfig，再取值；观察因未实现而失败。缺头文件／链接错误只记录为接口检查，不记作运行断言失败。
- [ ] **Step 3：建立依赖并实现配置。** 安装 WSL 开发包 `pkg-config libcurl4-openssl-dev libsqlite3-dev libgmime-3.0-dev libxml2-dev python3 openssl`；CMake 使用 `CURL::libcurl`、`SQLite::SQLite3`、`PkgConfig::GMIME`（模块 gmime-3.0）和 `LibXml2::LibXml2`，保持四个原目标及新增测试的 C++23 要求。库版本下限按 Tech Stack，依赖缺失时给出明确配置失败。数据目录解析与收信配置解析分开，配置只持有密码环境变量名称；内存列表投影同步保留新增状态字段。
- [ ] **Step 4：配置、构建并运行全部 CTest。** 按下方通用命令执行；原 inbox、cli、cli_smoke 与新增 config 全部通过、无编译警告。检查 libcurl 实际支持 IMAP/IMAPS 和 TLS，将实际版本与检查结果写入依赖记录，不宣称 MIME 样本已通过。
- [ ] **Step 5：检查并限定路径提交。** `feat: add mail runtime configuration`。

## Task 2：SQLite 缓存与可靠写入

**Files:** store、data_dir_lock 文件，`tests/store_test.cpp`；必要时扩展 `tests/inbox_test.cpp`。

**Interfaces:** 消费 Task 1 的类型；产出 SqliteStore 与 DataDirLock，供同步、CLI 装配调用。

- [ ] **Step 1：导师写存储测试。** 使用真实临时数据库，验证重新打开后内容与 ID 保留、相同 RemoteKey 重试 ID 不变、已知空账号成功、未知账号报错、事务失败不丢 pending。排序用测试辅助 SQL 在临时库写入固定 ID 42/43/44 和额外的 `id=7/time=50`；改变输入排列，固定期望 `42,43,44,7`，分别检查内存查询与存储查询，补充阶段 1 留下的排列覆盖。固定 ID 写入仅属于测试种子，生产路径仍由数据库生成 ID。
- [ ] **Step 2：建立最小存储接口，观察上述行为断言失败。** 新用例检查实际行数、查询结果和待处理项，不只检查“打开成功”。
- [ ] **Step 3：实现 SQLite 和写进程锁。** SQLite 使用短事务、参数绑定、外键与文件数据库 WAL；schema user_version=1，版本不支持／损坏时报告错误，禁止删除数据库后重建。写命令持有数据目录独占锁，默认最多等 5 秒；读缓存不拿写锁。网络请求期间不持有 SQLite 写事务。

建立 `accounts`、`sync_state`、`sync_pending`、`messages`、`attachments` 五张表。messages 的本地 ID 为 `INTEGER PRIMARY KEY AUTOINCREMENT`，远端唯一键为 `(account_id,folder,uid_validity,uid)`；原文存 BLOB，正文和头字段存 TEXT。禁止用 REPLACE 破坏已有 ID。SQLite 绑定前检查范围：合法 MailId 大于 INT64_MAX 时，find 返回 mail_not_found，不进行有符号转换。账号同 ID 下的服务器／端口／用户名改变要拒绝并保留原数据；显示名可以更新。

`schedule` 在同一事务注册新 pending 和发现进度；`complete` 在同一事务保存邮件／附件，只在解析成功时移除当前待办。解析失败也保存原文和可定位索引，标记 failed，保留解析重试项。`fail` 保存抓取失败原因，其中 remote_gone 将任务标记为不可重试，后续 pending 不再返回它；已有本地邮件不删除。UIDVALIDITY 改变时，旧邮件保留为历史、旧队列失效，新范围重新初始化。

- [ ] **Step 4：运行 store 与全套测试。** 增加 UINT64_MAX 查询、注入 SQLite 写失败、双写锁争用／释放后恢复、同账号配置身份改变、不同文件夹／范围相同 UID 不冲突的断言。预期全绿，失败事务后原邮件与 pending 均可查询。
- [ ] **Step 5：限定路径提交。** `feat: persist mail cache and sync work`。

## Task 3：中文 MIME 与 HTML 正文解析

**Files:** MIME／HTML 文件、`tests/mime_test.cpp`、`tests/fixtures/mail/*.eml`。

**Interfaces:** 消费原始 RFC 邮件字节，产出 `parse_message` 的 ParsedMail 或 parse_error；附件大小为解码后字节数。

- [ ] **Step 1：导师创建虚构邮件样本与测试。** plain_utf8 的主题／正文为“课程通知”／“请查看课程安排”；Base64 和 quoted-printable 各覆盖中文；GB18030 样本期望“中文测试”；alternative 同时含两种正文时选择 plain；HTML `<p>面试 &amp; 报到</p><script>ignored()</script>` 提取可见文字“面试 & 报到”，不含脚本；mixed 验证附件名“简历.pdf”、类型和长度；缺主题仍可读、主题为“(无主题)”；空原文与不支持字符集返回可读解析错误。
- [ ] **Step 2：运行 mime，确认有效中文样本尚未实现而失败。** 先检查成功分支，再检查精确文本，避免异常掩盖失败原因。
- [ ] **Step 3：实现 GMime 适配与 HTML 转文本。** GMime 负责邮件头、传输解码及字符集；适配层检查输出 UTF-8，未知字符集不输出乱码。HTML 用 libxml2，只提取文本、忽略 script/style，不请求外部资源。附件不作为正文，保留元数据和原始邮件。限制原文 20 MiB、MIME 嵌套 32 层、部件 256 个；超限明确失败。
- [ ] **Step 4：运行 mime 与全套测试。** 增加原文 20 MiB+1、嵌套 33 层、257 部件的超限回归，预期明确 parse_error。样本全部通过后，在依赖记录中固定本轮实际验证的 GMime/libxml2 版本。以上能力不能只依据库 README 宣称通过。
- [ ] **Step 5：限定路径提交。** `feat: parse Chinese MIME mail`。

## Task 4：只读 IMAP 接入

**Files:** imap 文件，`tests/imap_test.cpp`、`tests/imap_server.py`。

**Interfaces:** 消费 ImapSettings 与密码值，产出 ImapSource。snapshot 返回当前范围和 UID 列表；fetch 返回匹配 RemoteKey 的原文及 INTERNALDATE，不能只使用列表序号定位。

- [ ] **Step 1：导师建立本机 TLS IMAP 测试服务器。** C++ 测试调用实际 libcurl。覆盖正常读取、空箱、无效凭据、证书不受信任、超时、临时断连后重试、错误 UID／UIDVALIDITY、抓取前 UID 已消失（remote_gone）与内部日期。初始未读的邮件读取后仍未读；输出和错误不得包含虚构测试密码。
- [ ] **Step 2：运行 imap，观察正常收取尚未实现的失败。** 默认 CTest 只连接临时 localhost 服务器，不连接学校邮箱。证书由测试脚本在临时目录生成，生产证书校验保持开启。
- [ ] **Step 3：实现 libcurl 适配。** 会话 RAII；URL、用户名、密码分别设置，不把密码拼进 URL。使用受支持的 IMAP 命令与 BODY.PEEK[]；元数据和邮件字节分别处理，不从邮件正文搜索协议标识。认证失败不重试，临时网络错误按约定最多重试 2 次；超时参数可由测试缩短。重连后重新核对 UIDVALIDITY，范围变化时拒绝将旧键绑定到新正文。正文超过 20 MiB 即停止，保留可重试错误。
- [ ] **Step 4：运行 imap 与全套测试。** 同时检查收到的正文、读状态、错误类型与耗时边界；仅能建立 TCP 连接不算通过。
- [ ] **Step 5：限定路径提交。** `feat: fetch mail through IMAP`。

## Task 5：增量同步、去重与恢复

**Files:** sync 文件、`tests/sync_test.cpp`。

**Interfaces:** 消费实际 SqliteStore、MIME 与可替换的 ImapSource，产出 `sync_account`。此函数不打印；配置、进程锁和连接装配由下一任务负责。

- [ ] **Step 1：导师写编排测试。** 首次 UID=1..30 只安排 11..30；重复同步成功数为 0 且行数不变；UID 10 失败、11 成功后关闭数据库，下一轮成功重试 10 并收取 12，最终三封且 11 的 ID 不变；发现队列已提交但尚未抓取就重启，任务继续；解析坏项保存 failed 索引，其他邮件继续；待办 UID 从远端消失时记录 remote_gone，下一轮不无限重试；UIDVALIDITY 100→200 保留旧邮件为历史。用固定预期数值，不复用被测算法生成答案。
- [ ] **Step 2：运行 sync，确认恢复或成功入库的行为尚未实现而失败。** 网络边界使用按 RemoteKey 返回固定字节／错误的测试源，SQLite 和 MIME 使用真实实现。
- [ ] **Step 3：实现发现→入队→抓取／解析→逐项事务保存。** 快照先去重并按 UID 排序；首次只排最近 20 个。发现游标只与持久待办一起提交，不用“最大成功 UID”代替。每轮先处理当前范围待办；临时抓取失败保留任务，解析失败保存 raw 与失败状态，下轮可重新解析；成功项完成后不再下载。当前完整快照已无该 UID 或抓取明确返回 remote_gone 时，记录一次失败并停用该待办，保留本地历史。持久化整体失败立即终止，禁止继续推进进度。
- [ ] **Step 4：运行 sync 与全套测试。** 空箱／无新邮件 summary 为 success=0、failed=0；全失败计为失败，部分成功有逐 UID 原因；最后完全成功时间只在本轮无失败时更新。
- [ ] **Step 5：限定路径提交。** `feat: resume incremental mail sync`。

## Task 6：真实缓存与 CLI 接入

**Files:** cli、runtime、main 文件，`tests/cli_test.cpp`、`tests/runtime_test.cpp`、`tests/cli_smoke.cmake`、README、CMake。

**Interfaces:** CLI 改为消费服务函数集合，避免查询依赖先加载整箱正文；保留核心内存查询供 demo 使用：

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

服务集合及回调拥有捕获值；创建服务时不打开文件／网络。main 识别并移除开头的 --demo，其他参数仍由 run_cli 校验；真实服务在被调用时才解析所需配置、打开资源。读命令只读 SQLite，不读取密码；sync 校验配置后持有写锁、打开存储、创建连接并调用同步服务。

- [ ] **Step 1：导师保留现有 31 个 CLI 用例并适配调用入口，增加新测试。** 缓存查询在无密码且网络源不可用时仍成功；无缓存显示 0 封，不混入样例；--demo 行为保留；--demo sync、sync 缺失／重复账号及未知参数返回 2 且不调用服务；同步全成功／全失败／部分成功分别返回 0／1／3，部分失败展示计数和 UID 原因。UINT64_MAX 查询仍为未找到 1，溢出仍为参数错误 2。
- [ ] **Step 2：运行 cli、runtime 与全套测试。** 观察当前入口不支持服务装配、新命令或来源显示的明确失败；正常参数完成校验之前，不得访问存储或网络。
- [ ] **Step 3：实现服务装配与显示。** inbox 使用 SQLite 列表投影，show 才读取正文与附件；解析失败与历史状态明确显示。真实文本进入终端前处理 ESC 等控制字符；列表字段中的换行／制表符不能伪造新行，正文保留正常换行。原文和缓存内容保持原值。帮助与 README 说明环境变量、来源模式及 0/1/2/3 退出码。
- [ ] **Step 4：接入 CMake 并运行全套。** 保留 inbox、cli、cli_smoke，新增 config、store、mime、imap、sync、runtime，共 9 项；cli_smoke 显式执行 --demo inbox，运行期间不依赖真实账号。检查虚构主题含 ESC／换行时终端无原始 ESC，UTF-8 中文仍完整；缺配置的帮助和离线查询不触发收信。
- [ ] **Step 5：限定路径提交。** `feat: connect CLI to persistent mail cache`。

## Task 7：真实邮箱验收与阶段交付

**Files:** `tests/live_mailbox_test.cpp`、`README.md`、`docs/dependencies.md` 与本计划的验收记录；必要修复按失败用例先行。

- [ ] **Step 1：核对本人客户端设置。** 用户在本机 WSL 环境按学校说明生成客户端专用密码，设置上表变量；真实验收数据目录显式指定为项目 build/live-data 或其他不入 Git 的目录。命令在已配置的同一 WSL 会话运行，或由导师以明确加载本机环境的 WSL 包装调用；记录证据来源。只记录参数来源、依赖版本和验收结果，不记录密码及真实邮件正文。
- [ ] **Step 2：连接验证。** 导师维护独立 live_mailbox_test 目标，复用真实存储、网络及同步入口，以 SyncOptions{5} 检查配置；只在显式设置 MAILAI_LIVE_TESTS=1 时运行，不注册默认 CTest。探针使用独立的 build/live-probe 数据目录，输出数量和状态。正式验收使用另一数据目录和默认 20 封首次策略。读取前后在本人邮箱界面对照已读状态。端点不可达或认证失败时，记录实际错误，不能把离线测试当作真实接入成功。
- [ ] **Step 3：真实同步与重启。** 运行 sync → inbox → show → 退出重启 → 再次 inbox/show → 重复 sync；确认稳定 ID、可读中文、无重复行。移除本机密码变量后，inbox/show 仍可读取缓存。部分失败、崩溃恢复与 UIDVALIDITY 变化使用前述可控测试验证，不修改真实服务器状态制造故障。
- [ ] **Step 4：Debug 与独立 Release 目录完整验证。** 运行全部 9 项 CTest，检查无编译警告、无真实网络依赖、原有功能不回归；更新依赖与真实验收记录，独立只读审阅整个阶段，有重要问题则先写失败回归再修复。
- [ ] **Step 5：限定路径提交并更新进度。** `docs: record single mailbox acceptance`。真实验收受外部配置阻塞时明确记录尚未完成，不将阶段 2 标记为全部完成。

## 通用验证命令

每个任务只增加自己的目标／测试；执行以下命令，预期配置、构建和全部已接入 CTest 均退出 0，无编译警告。测试未通过时先定位原因，不跳过失败项。

```powershell
wsl.exe -d Ubuntu -- cmake -S /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant -B /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
wsl.exe -d Ubuntu -- cmake --build /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug
wsl.exe -d Ubuntu -- ctest --test-dir /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug --output-on-failure
```

阶段结束时使用新的 `build/verify-stage2-release` 目录，把上述构建目录替换并指定 `-DCMAKE_BUILD_TYPE=Release`，预期九项全通过。若目录已存在，选择新的无日期名称，保留已有产物。

## 自审与衔接

- 阶段 2 的认证、IMAP、MIME、SQLite、进度、去重与重启恢复分别落在 Task 1/4、4、3、2、2/5、2/5、2/5/7。
- 所有后续任务消费的类型与函数集中在接口约定中；Task 1 先定义共享值类型，生产解析／网络函数在所属任务实现。
- Review Focus 五类输入均指定了负责测试的任务；阶段 1 的排序排列覆盖纳入 Task 2。
- 本轮只写计划并做只读资料／环境检查，没有安装开发包或连接真实邮箱。
- 用户已审阅通过，从 Task 1 开始；按本轮要求让用户参与测试编写，业务实现继续带写。

## 一手资料与选型依据

- [北交大客户端专用密码教程](https://ic.bjtu.edu.cn/fwzn/xxxtzn/202311421.htm)：确认客户端凭据的取得方式，实际连接参数仍按本人设置页核对。
- [libcurl IMAP 示例](https://curl.se/libcurl/c/imap-fetch.html)、[自定义请求说明](https://curl.se/libcurl/c/CURLOPT_CUSTOMREQUEST.html)：网络适配依据；示例不能直接替代只读与错误处理验证。
- [RFC 9051](https://www.rfc-editor.org/rfc/rfc9051.html)：UID／UIDVALIDITY 标识及 BODY.PEEK 的只读语义；连接实现兼容学校服务器实际支持的 IMAP 版本。
- [SQLite 数据类型](https://www.sqlite.org/datatype3.html)：INTEGER 的有符号范围与 BLOB；公开无符号 MailId 在边界处显式处理。
- [GMime 项目](https://github.com/jstedfast/gmime)、[文本部件实现](https://github.com/jstedfast/gmime/blob/master/gmime/gmime-text-part.c)：候选解析与字符集处理依据，仍须通过本计划样本验证。
- [libxml2 HTML API](https://gnome.pages.gitlab.gnome.org/libxml2/html/HTMLparser_8h.html)：HTML 文本提取依据，不实现网页渲染。
- [mailio 上游问题报告 #227](https://github.com/karastojko/mailio/issues/227)：原候选调整的依据之一；只确认存在报告，未在本工程复现。

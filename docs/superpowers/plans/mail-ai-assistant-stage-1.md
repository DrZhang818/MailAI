# Mail AI Assistant Stage 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付可构建的 C++23 核心库与终端程序，用虚构邮件完成列表、账号筛选和详情查看。

**Architecture:** `mail_assistant` 负责参数与显示，`mailai_core` 返回查询结果或错误。数据以拥有字符串的值类型传递，查询与显示分开。

**Tech Stack:** C++23、CMake 3.20+、Ninja、CTest、WSL Ubuntu；本阶段只使用标准库。

工具链检查：WSL 中 GCC 15.2.0、CMake 4.2.3；`g++ -std=c++23` 预处理模式报告 `__cplusplus=202302L`。该检查验证标准模式可用，工程构建与测试按下方步骤另行验收。

**Spec:** [已批准的设计规格](../specs/mail-ai-assistant-design.md)，本计划实现其第 9 节阶段 1。

## Global Constraints

- 新文件按主题命名，不加日期前缀；当前专注本项目，暂停维护原实习计划和 CCG 学习记录。

- 语言标准采用 C++23；核心库公开 `cxx_std_23` 编译要求。所有项目目标设置 `CXX_STANDARD 23`、`CXX_STANDARD_REQUIRED ON`、`CXX_EXTENSIONS OFF`。CMake 最低版本为 3.20，依据 [CMake 官方文档](https://cmake.org/cmake/help/latest/prop_tgt/CXX_STANDARD.html)。

- 测试、测试辅助与验证由导师直接维护；业务实现由导师在终端给出完整参考和必要解释，用户理解后抄入源码。导师检查实际代码并验证，不把参考代码的提供记为用户已独立实现。上方 agentic worker 说明只适用于以后明确委托智能体直接修改业务源码的情况。
- 本轮用户要求“你来继续做吧”，授权导师直接完成 Task 2 剩余的 CMake 接入、文档、格式整理、验收和本地提交；后续业务实现默认仍沿用带写方式。
- “一个可执行程序 `mail_assistant` 调用一个核心库 `mailai_core`。”
- “依赖方向：CLI → 业务服务 → 邮件／AI／存储。”本阶段完成 CLI 与查询业务入口。
- “持久记录拥有自己的字符串和容器。”记录成员使用 `std::string`，业务值类型采用成员默认操作。
- “工程使用 CMake、CTest 和 WSL Ubuntu；构建与运行通过 `wsl.exe -d Ubuntu -- ...` 调用 WSL 工具。”
- “正常结果写 stdout，进度和诊断写 stderr”；本阶段成功 0、运行失败 1、参数错误 2。
- “示例 ID 为虚构数据。”成功展示标明“数据来源：示例”，不将演示记录记为真实邮件。

## Review Focus

1. 已知账号没有邮件：返回成功的空结果，列表显示 0 封；Task 1 空列表测试与 Task 2 空列表展示测试。
2. 邮件时间相同、输入顺序改变：仍按时间降序、同时间 ID 升序；Task 1 排序测试。
3. 按账号筛选后读取详情：`show` 使用稳定邮件 ID，始终保留来源账号；Task 1 查找测试与 Task 2 筛选／详情测试。
4. 缺失、重复参数，或 ID 为负、溢出、含尾随字符：退出码 2、stdout 为空、stderr 有原因；Task 2 参数表测试。
5. 中文主题与正文、查无账号／邮件：中文原样展示，业务失败退出码 1；Task 1 错误测试与 Task 2 中文／错误测试。

---

## 范围与后续阶段

现有工程只有设计与调研文档。本计划只创建阶段 1 所需文件。阶段 2 再扩展连接、协议字段及 SQLite；阶段 3 接云端 AI 与汇报；阶段 4 做双账号与 watch；阶段 5 做草稿和发送状态；阶段 6 做一键批量；阶段 7 完成交付。各阶段任务与验收沿用规格第 9 节，在前一阶段接口验收后分别编写详细计划。

阶段 1 的 `show` 和筛选用于验证稳定 ID、账号归属及 CLI 边界。读取数据来自内存中的示例；真实同步、数据库、模型、回复和 `--json` 属于后续阶段。

## 文件结构与责任

以下均相对于 `projects/mail-ai-assistant/`。业务文件由用户按终端参考创建，测试及测试辅助文件由导师直接维护。

| 文件 | 责任 | 任务 |
|---|---|---|
| `CMakeLists.txt` | 核心库、程序与测试目标；按目标启用 C++23 和警告 | 1、2 |
| `.gitignore` | 排除 `build/`、本地凭据及运行数据 | 1 |
| `include/mailai/model.h` | Account、Mail、Error、Result 定义 | 1 |
| `include/mailai/inbox.h` | 列表查询参数、列表项、查询接口 | 1 |
| `src/app/inbox.cpp` | 账号筛选、稳定排序、按 ID 查找 | 1 |
| `tests/test_support.h` | 测试条件失败时抛异常，Release 下也执行检查 | 1 |
| `tests/inbox_test.cpp` | 核心查询与错误场景 | 1 |
| `src/cli.h`、`src/cli.cpp` | 接受参数与数据、返回退出码、写入指定输出流 | 2 |
| `src/main.cpp` | 构造示例、转换 argv、调用 CLI | 2 |
| `tests/cli_test.cpp` | 注入数据与输出流验证 CLI 行为 | 2 |
| `tests/cli_smoke.cmake` | 执行真实程序，检查退出码及示例内容 | 2 |
| `README.md` | 本阶段支持范围、构建、测试、演示命令 | 1、2 |

## Task 1：可构建的核心查询

**Files:** 创建上表任务 1 文件。交付独立可运行的核心测试。

**Interfaces:** 全部位于 `mailai` 命名空间，公共头文件自包含。

```cpp
using AccountId = std::string;
using MailId = std::uint64_t;
template<class T> using Result = std::variant<T, Error>;

Result<std::vector<InboxItem>> list_inbox(
    const std::vector<Account>& accounts,
    const std::vector<Mail>& mails, const InboxQuery& query);
Result<Mail> find_mail(const std::vector<Mail>& mails, MailId id);
```

`model.h` 先定义 Error，再声明 Result。字段按下表顺序定义；后续阶段按需要扩展协议和存储字段。

用户将 MailId 改为 `std::uint64_t`，保留 0 为无效 ID；已增加最大无符号 ID 的查找测试，CLI 后续覆盖完整无符号范围及精确溢出边界。用户已将 Mail 与 InboxItem 的时间戳统一为 `std::int64_t`。阶段 2 需在 SQLite 整数绑定前明确检查取值范围，避免有符号转换截断。

| 类型 | 本阶段成员 |
|---|---|
| Account | `AccountId id; std::string display_name, address;` |
| Mail | `MailId id; AccountId account_id; std::string sender, subject, body; std::int64_t received_at_utc;` |
| ErrorCode | 枚举 `unknown_account, mail_not_found, invalid_argument` |
| Error | `ErrorCode code; std::string message; std::optional<AccountId> account_id; std::optional<MailId> mail_id;` |
| InboxQuery | `std::optional<AccountId> account_id;`，空值表示全部账号 |
| InboxItem | `MailId id; AccountId account_id; std::string account_name, sender, subject; std::int64_t received_at_utc;` |

测试／演示数据：账号 `school / 学校邮箱 / student@school.example`、`qq / QQ邮箱 / student@qq.example`；邮件按下表倒序输入，时间单位为 UTC epoch 秒。

| id | account_id | sender | subject | body | received_at_utc |
|---|---|---|---|---|---:|
| 42 | school | course@school.example | 课程通知 | 请查看课程安排 | 200 |
| 43 | qq | recruiter@example.com | 实习通知 | 请确认面试时间 | 200 |
| 44 | school | library@school.example | 图书馆通知 | 请按时归还图书 | 100 |

- [x] **Step 1：写核心测试。** 创建测试入口及 `test_support.h` 的 `void expect(bool condition, const char* message)`；条件失败抛异常，测试入口捕获后返回 1。按表构造数据，写入以下断言；正常结束返回 0。

执行顺序：以下为 Step 1 的完整验收集合。用户已委托导师直接维护测试，导师已将整组测试写入 `tests/inbox_test.cpp`，保留已有 `test_support.h`。当前九组测试包含全部计划断言以及来源账号、空账号集合、错误上下文和无符号最大 ID 检查，使用与下方示例一致的 `std::get`。补充检查未知账号即使没有邮件也报错；邮件引用不存在的账号时返回 unknown_account，携带账号 ID 与邮件 ID，避免解引用无效迭代器。本项勾选表示测试已编写并核对覆盖，不表示测试已运行成功。

```cpp
// accounts、mails 按上表构造；输入 mails 顺序为 44、43、42。
const auto all = std::get<std::vector<InboxItem>>(
    list_inbox(accounts, mails, InboxQuery{}));
expect(all.size() == 3, "all count");
expect(all[0].id == 42 && all[1].id == 43 && all[2].id == 44, "stable order");
const auto school = std::get<std::vector<InboxItem>>(
    list_inbox(accounts, mails, InboxQuery{AccountId{"school"}}));
expect(school.size() == 2 && school[0].id == 42 && school[1].id == 44, "account filter");
const auto mail = std::get<Mail>(find_mail(mails, 43));
expect(mail.id == 43 && mail.account_id == "qq" && mail.subject == "实习通知", "stable id");
expect(std::get<std::vector<InboxItem>>(
    list_inbox(accounts, {}, InboxQuery{AccountId{"school"}})).empty(), "empty success");
expect(std::get<Error>(list_inbox(accounts, mails, InboxQuery{AccountId{"missing"}})).code
    == ErrorCode::unknown_account, "unknown account");
expect(std::get<Error>(find_mail(mails, 999)).code == ErrorCode::mail_not_found, "missing mail");
expect(std::get<Error>(find_mail(mails, 0)).code == ErrorCode::invalid_argument, "invalid id");
```

另检查列表项的 `account_id`、`account_name` 与对应邮件一致，空账号集合＋空邮件集合返回空成功；未知账号错误带请求的账号 ID，未找到邮件错误带请求的邮件 ID。

- [x] **Step 2：确认红灯。** 从 WSL 执行下面的核心测试编译命令。预期非零退出，诊断为尚缺 `mailai/inbox.h` 或查询声明／定义；工具未安装等环境错误先记录并修复，不作为测试红灯证据。

```bash
wsl.exe -d Ubuntu -- g++ -std=c++23 -Wall -Wextra -Wpedantic -I/mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/include /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/tests/inbox_test.cpp -o /tmp/mailai-inbox-red
```

实际验证：最初上述命令退出码为 1，诊断为 `fatal error: mailai/inbox.h: No such file or directory`，符合本步骤的接口缺失检查。用户写入两个头文件后，同一命令通过源文件编译，在链接阶段因 list_inbox 与 find_mail 尚无定义而失败，退出码为 1。这两次均为接口／实现缺失检查，没有运行任何业务断言；补齐实现后的实际测试记录见文末当前进度。

- [x] **Step 3：实现模型与查询。** 在指定头文件定义上表类型，在 `src/app/inbox.cpp` 实现两项接口：先验证请求账号，再筛选并关联账号名称，按时间降序／同时间 ID 升序排序；`find_mail` 按正数稳定 ID 查找。返回拥有数据的结果，输入保持原值。以 `std::variant` 区分空成功与错误。

- [x] **Step 4：建立构建并验绿。** `CMakeLists.txt` 最低版本 3.20，项目名 `mail_ai_assistant`；创建静态库 `mailai_core`、可执行测试 `inbox_test`、CTest 名称 `inbox`。公开 include 目录和 `cxx_std_23`，所有目标设置 `CXX_STANDARD 23`、`CXX_STANDARD_REQUIRED ON`、`CXX_EXTENSIONS OFF`；自身目标使用 `-Wall -Wextra -Wpedantic`。在 README 保存本阶段命令，`.gitignore` 排除 `build/`、`credentials.local.*`、`*.sqlite*`。逐条执行：

```bash
wsl.exe -d Ubuntu -- cmake -S /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant -B /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
wsl.exe -d Ubuntu -- cmake --build /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug
wsl.exe -d Ubuntu -- ctest --test-dir /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug -R '^inbox$' --output-on-failure
```

预期：构建无警告，`inbox` 通过，`100% tests passed`，三条命令退出码均为 0。

- [x] **Step 5：审查并提交。** 用户说明空结果与错误、稳定 ID 与行号、字符串所有权的区别；导师核对实际代码和上述输出。只暂存本任务表列出的文件，提交信息 `feat: add in-memory mail queries`。

执行记录：用户已完成三项复盘；导师补充 list_inbox 的空成功类型为 vector<InboxItem>，底层存储失效后 string_view 悬空、继续访问可能产生未定义行为。项目名与行尾空格已由用户修正；重新配置、构建及运行全部现有 CTest，均退出 0，inbox 1/1 通过、编译无警告。八个任务文件已提交为 `4960d4a`（`feat: add in-memory mail queries`）。使用限定路径的提交保留了用户其余已有暂存修改。

## Task 2：CLI 列表、详情与错误反馈

**Files:** 创建 `src/cli.h`、`src/cli.cpp`、`src/main.cpp`、`tests/cli_test.cpp`；修改 `CMakeLists.txt` 与 `README.md`。

**Consumes:** Task 1 的 Account、Mail、InboxQuery、Result、ErrorCode、`list_inbox`、`find_mail`、`expect`。

**Produces:** `src/cli.h` 声明下列 `mailai::cli` 接口；`args` 不含程序名，输出流由调用者提供。

```cpp
int run_cli(const std::vector<std::string>& args,
            const std::vector<Account>& accounts,
            const std::vector<Mail>& mails,
            std::ostream& out, std::ostream& err);
```

- [x] **Step 1：写 CLI 测试。** 使用 Task 1 的数据和 `std::ostringstream` 捕获两路输出。每行单独调用 `run_cli`，检查退出码及表中内容；空列表用原账号和空 mails 注入。

| args / 数据 | 断言 |
|---|---|
| `{}`、`{"--help"}`、`{"inbox","--help"}`、`{"show","--help"}` | 0；stdout 含 `Usage:`、`inbox`、`show`；stderr 为空 |
| `{"inbox"}`、`{"inbox","--account","all"}` | 0；stdout 含“数据来源：示例”、42／43／44、学校邮箱／QQ邮箱、课程通知／实习通知；stderr 为空 |
| `{"inbox","--account","school"}` | 0；有课程通知和图书馆通知，无实习通知 |
| `{"inbox"}`＋空 mails | 0；stdout 含“0 封”；stderr 为空 |
| `{"show","43"}` | 0；stdout 含 43、qq、实习通知、请确认面试时间；stderr 为空 |
| `{"inbox","--account","missing"}`、`{"show","999"}` | 1；stdout 为空，stderr 分别含 missing／999 |
| `{"show","18446744073709551615"}` | 正确解析为最大 MailId；当前样本无此 ID，退出 1，stdout 为空，stderr 含该 ID |
| `{"inbox","--account"}`、`{"inbox","--account","school","--account","qq"}` | 2；stdout 为空；stderr 非空 |
| `{"show"}`、`{"show","42","43"}`、`{"unknown"}`、`{"inbox","--unexpected"}` | 2；stdout 为空；stderr 非空 |
| `{"show",id}`，id 分别为 `""`、`"0"`、`"-1"`、`"+42"`、`"42x"`、`"18446744073709551616"`、`"999999999999999999999999"` | 2；stdout 为空；stderr 非空 |

断言写法使用 Step 1 已定义的 `expect`，例如 `expect(code == 2 && out.str().empty() && !err.str().empty(), "bad arguments")`；每项使用新的输出流。导师已创建 tests/cli_test.cpp，七组测试共 27 次调用，覆盖整张表，并补充先完整校验参数再执行可能失败的查询、列表发件人与数量及展示顺序检查。本项勾选表示测试已编写，不表示 CLI 断言已经通过。

- [x] **Step 2：确认红灯。** 执行下列命令，预期因尚缺 cli.h／run_cli 非零失败；记录诊断。

```bash
wsl.exe -d Ubuntu -- g++ -std=c++23 -Wall -Wextra -Wpedantic -I/mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/include -I/mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/src /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/tests/cli_test.cpp -o /tmp/mailai-cli-red
```

实际记录：上述命令退出码 1，诊断为 `fatal error: cli.h: No such file or directory`。这验证接口尚未建立，没有运行任何 CLI 业务断言，后续实现完成后再检查运行结果。

- [x] **Step 3：实现 CLI 和程序入口。** `run_cli` 支持表中命令；使用 `std::from_chars` 完整解析十进制正数 MailId，参数完整校验后才调用查询。账号 `all` 映射为无筛选，重复选项和多余位置参数报错。列表展示稳定 ID、账号名、发件人、主题及数量；详情展示原 ID、账号 ID、发件人、主题与正文。核心错误写 stderr 并返回 1，参数错误返回 2。`main.cpp` 构造上表三封示例，排除 argv[0] 后调用 `run_cli`；未预期的 `std::exception` 由入口报告 stderr 并返回 1。

- [x] **Step 4：接入并验绿。** 新增程序目标 `mail_assistant` 和测试目标 `cli_test`；各自编译 `src/cli.cpp` 并链接 `mailai_core`，`src` 仅作为相应目标私有 include 路径。CTest 新增 `cli` 与运行 `mail_assistant inbox` 的 `cli_smoke`，后者要求正常退出且输出含“课程通知”。重新执行 Task 1 Step 4 的配置／构建命令，CTest 去掉 `-R '^inbox$'`；预期 inbox、cli、cli_smoke 三项通过，退出码 0。

测试辅助由导师维护：已创建 tests/cli_smoke.cmake，使用 execute_process 执行实际程序，同时检查退出码为 0 和 stdout 含课程通知。CTest 通过 cmake -DMAIL_ASSISTANT=$<TARGET_FILE:mail_assistant> -P 调用该脚本，路径随构建目录解析，适用于 Debug 与 Release。

- [x] **Step 5：终端验收、文档和提交。** 运行下列命令并核对与测试表相同的结果；README 记录阶段范围、示例标记、退出码及构建步骤。再从新的 Release 构建目录执行配置、构建、CTest，确认测试检查仍生效。只暂存本任务修改文件，提交信息 `feat: add demo mailbox CLI`。

```bash
wsl.exe -d Ubuntu -- /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug/mail_assistant inbox
wsl.exe -d Ubuntu -- /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug/mail_assistant inbox --account school
wsl.exe -d Ubuntu -- /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug/mail_assistant show 43
wsl.exe -d Ubuntu -- /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug/mail_assistant show 42x
```

前三条预期退出码 0，最后一条预期退出码 2。首次使用以下 Release 验证目录；若目录已存在则统一改用新的目录名，保留已有产物。

```bash
wsl.exe -d Ubuntu -- cmake -S /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant -B /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/verify-stage1-release -G Ninja -DCMAKE_BUILD_TYPE=Release
wsl.exe -d Ubuntu -- cmake --build /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/verify-stage1-release
wsl.exe -d Ubuntu -- ctest --test-dir /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/verify-stage1-release --output-on-failure
```

预期构建退出 0、无警告，inbox、cli、cli_smoke 三项均通过。

执行记录：导师按本轮委托直接完成 CMake、README 与三处行尾空白整理。Debug 以及首次创建的 build/verify-stage1-release 均配置、构建成功且无警告，全部 CTest 各 3/3 通过；Release 编译使用 -O3 -DNDEBUG，测试条件检查仍执行。四条终端验收命令实际退出码依次为 0、0、0、2，列表为 42/43/44，学校筛选为 42/44，show 43 保留 qq 来源及中文正文。七个 Task 2 文件已限定路径提交为 `4a67e13`（`feat: add demo mailbox CLI`）；其他用户已有暂存内容保持原状。独立整体审阅发现的重要参数问题已按先失败再修复验证，修复另提交为 `f4948ab`，详见下方审阅记录。

## 阶段 1 整体审阅

独立只读审阅范围为 08ea297..4a67e13，覆盖阶段 1 的 13 个实现、测试与构建文档文件。无 Critical，1 个 Important，1 个 Minor；审阅者为 stage1_code_review。

- Final: fixed：`--account` 后提供空字符串或另一长选项时，原来被误判为未知账号而退出 1。增加 test_account_option_requires_value，四种输入分别为空值、--account、--unexpected、--help；修改实现前，Debug 的 cli CTest 退出 8，断言明确失败。随后在查询前检查空值和长选项形式的值，返回参数错误 2；正常未知账号仍返回 1。Debug/Release 均重建且无警告，全套 CTest 各 3/3 通过；CLI 当前为 8 组、31 次调用。三项修复文件已提交为 f4948ab（fix: reject missing account option values）。未再次派发审阅。
- Final: minor (deferred)：可为排序补充不同输入排列的回归测试。当前比较器正确，既定排序与同时间 ID 规则检查通过；更多排列覆盖留作后续测试增强。
- Final: Ruling：真实邮箱、数据库、AI、回复与 JSON 沿用后续阶段安排——本阶段交付为明确标记的虚构邮件 CLI；若误判阶段范围，会把示例误当作已具备真实邮箱功能，因此 README 已明确当前范围。
- Final: Ruling：重复账号／邮件 ID 和零 ID 记录的导入校验留到真实数据入口阶段——本阶段只有固定可信样例，尚无导入或创建入口；未来开放入口前必须补充校验，防止身份歧义。
- Final: Ruling：平台验收采用 WSL Ubuntu GCC，不声明 MSVC 原生支持——符合已批准工具链；原生 Windows 编译需另行适配与验证。
- Final: Ruling：输出设备故障恢复不纳入本阶段终端样例交付——当前未实现专门的流故障处理；未来若依赖重定向输出的可靠性，需要补充失败诊断和退出码检查。
- Final: Ruling：现有未纳入任务的暂存文档和编辑器配置保留原状——提交始终限定为任务文件；其余修改仍需由用户按自己的节奏提交。

## 计划审阅与执行

本计划已由用户审阅通过。测试、测试辅助与验证由导师直接维护；业务实现按“终端给出完整参考代码 → 用户理解并抄入文件 → 导师检查 → 实际验证 → 记录与提交”推进。先推进 Task 1，再推进 Task 2。自动修改业务源码仍需用户另行明确委托；整项任务的自动实现调度不属于当前导师指导范围。

当前进度：阶段 1 完成。Task 1、Task 2 和审阅修复已本地提交（4960d4a、4a67e13、f4948ab）。最终 Debug/Release 全部 CTest 各 3/3 通过，无编译警告，CLI 覆盖 31 次调用；实际终端验收符合预期。保留一项非阻塞的排序排列测试增强。未接入真实邮箱、持久化或 AI，未启动阶段 2 实现。

格式检查：用户已清理两处行尾空格。Task 1 限定路径的暂存区格式检查通过；提交后工作区 `git diff --check` 通过。

接口预检：Task 1 产出的 Account、Mail、InboxQuery、Result、ErrorCode、`list_inbox`、`find_mail` 和测试辅助函数均与 Task 2 的输入一致，未发现接口命名或参数冲突。用户实现后的检查、编译与测试结果在此更新；原实习计划与 CCG 记录继续暂停维护。

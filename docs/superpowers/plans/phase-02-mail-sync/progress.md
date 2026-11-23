# 阶段 2 进度

对应[阶段计划](plan.md)。阶段尚未整体验收。表中保留先前验收记录，并在完成后补充新的实际验证结果。

| 任务 | 状态 | 既有结果与证据 |
|---|---|---|
| Task 1：配置、值类型与依赖 | 完成 | 提交 `874e0a6`；原记录确认配置、共享类型及基础依赖接入完成，实际版本见[依赖记录](../../../dependencies.md)。 |
| Task 2：数据库生命周期 | 完成 | 原生命周期记录确认单版本策略通过 Debug／Release 各 5 项 CTest；不代表邮件写入和同步完成。 |
| Task 2：邮件列表及详情读取 | 完成 | 原列表与详情记录确认 Debug／Release 各 7 项 CTest 通过，无构建警告；接口尚未接入 CLI。 |
| Task 2：同步状态读取 | 完成 | 原同步状态记录确认 Debug／Release 各 8 项 CTest 通过，格式与差异检查通过。 |
| Task 2：读取仍需重试的邮件 | 完成 | 用户按参考代码写入 `pending`，导师检查后在实际项目构建并运行全套 CTest，macOS Debug／Release 各 8/8 通过，构建无警告；筛选、排序、空结果、参数错误、整数边界、完整字符串及失败恢复测试通过。 |
| Task 2：登记邮件任务（schedule） | 完成 | 用户按参考代码完成并修正参数绑定和 SQL 拼写。实际项目 macOS Debug／Release 全套 CTest 各 8/8 通过，构建无警告；首次范围、去重、旧任务保留、进度不倒退、UIDVALIDITY 变化、账号与文件夹隔离、失败回滚及解除故障后重试均通过测试。 |
| Task 2：邮件写入、失败处理与同步结果 | 完成 | 经用户委托，由助手直接实现 `complete`、`fail` 和 `record_outcome`，并维护 `store_write` 测试。实际项目 macOS Debug／Release 全套 CTest 各 9/9 通过，构建无警告；邮件与附件原子保存、稳定 ID、解析失败保存与恢复、远端消失后停止重试、同步时间记录、重开持久性、只读拒绝写入及注入故障后的回滚与重试均通过验证。复查发现的迟到解析失败重新激活已停止任务的问题已修正，并加入回归测试。 |
| Task 2：内部 SQL 操作整理 | 完成 | 按用户选择统一为内部 `SqlContext`，移除业务函数中的 SQL 封装 lambda 和文件私有 `WriteContext`，复用文本读取。SQL 字符串、公开接口及事务边界保持一致；新增 `sqlite_context` 验证参数复制、空值、语句复用、执行错误及提交失败恢复，实际 Debug／Release 各 10/10 通过，构建无警告，格式与差异检查通过，独立审阅未发现阻碍问题。 |
| Task 2：数据目录写锁 | 组件完成 | 用户已按参考代码完成 `DataDirLock` 并补齐析构、移动和超时判断，导师接入核心库、Threads 依赖及 `data_dir_lock` CTest。实际项目在受管理临时目录完成 macOS Debug／Release 独立构建，各 11/11 项测试通过，构建无警告。写锁测试覆盖跨进程争锁、默认与自定义超时、等待后取得、进程退出和对象移动后的释放、已有文件及数据保留、参数与文件系统错误，以及持锁期间的只读缓存访问。Linux 本轮未执行。写命令入口按 Task 6 第 4 步接入。 |
| Task 2：存储回归 | 完成 | 本轮 macOS Debug／Release 各 11/11 项全套 CTest 通过，存储测试重新验证打开与结构检查、查询与只读访问、同步任务登记、邮件写入、重开持久性及失败回滚恢复。Task 2 的组件实现与存储验收已完成。 |
| Task 2：类型化结果读取与业务操作重构 | 完成 | 按用户选择仅重构存储层。`SqlRow` 统一字段类型、NULL、整数范围和布尔值检查，`query_one`、`query_all`、`scan` 统一结果行处理，命名 decoder 集中映射业务记录。模板声明和定义分置于 `.h`／`.hpp`，语句准备与首次绑定合并为私有 `_prepare`，`Statement` 及其绑定、推进、执行操作全部收为私有辅助方法，调用方仅传 SQL 和参数。新增 `execute_many` 接收固定参数和每行参数范围，一次准备、多次执行，批量操作沿用业务调用者的事务；`schedule` 的任务登记和附件写入均使用该入口。schema 与配置 SQL 统一经 `SqlContext` 执行，`complete`、`schedule` 拆为职责明确的内部操作，共享同步状态查询使用调用者错误上下文。新增查询和批量接口测试均先因接口缺失编译失败，实现后通过，并补充参数复制、每行完整绑定、空批次、失败回滚恢复及列表和详情非法存储类型回归。实际项目 macOS Debug／Release 各 11/11 项 CTest 通过，构建无警告。独立只读审阅未发现实质问题，核对公开接口、全部 19 个 SQL 原始字符串内容和冻结阶段计划均保持一致。Linux 本轮未执行。 |
| Task 3：顶层和直接子部件的纯文本选择 | 子步骤通过 | 用户按参考代码完成部件选择。macOS Debug 重新构建后，CTest 12/12 通过。MIME 测试验证了 UTF-8、GB18030 与 Base64 样本，以及 HTML 在前时仍选中纯文本的 `multipart/alternative` 样本。 |
| Task 3：多个纯文本版本的选择 | 子步骤通过 | 用户按参考代码实现 `multipart/alternative` 倒序选择。macOS Debug 重新构建后，CTest 12/12 通过。新增样本包含两个 `text/plain` 和末尾的 HTML，结果为靠后的 `text/plain`。 |
| Task 3：合并 `multipart/mixed` 的直接正文部件 | 子步骤通过 | 用户按参考代码实现正文选择与逐段解码。macOS Debug 重新构建后，CTest 12/12 通过。MIME 测试的五个样本通过，`multipart/mixed` 样本验证了两段纯文本按原顺序拼接，中间的文本附件被跳过。 |
| Task 3：读取邮件头 | 子步骤通过 | 用户完成邮件头读取。macOS Debug 和 Release 重新构建后，CTest 各 12/12 通过。样本验证了中文发件人和主题、三个会话字段、折行的 References，以及邮件头缺失时仍能读取正文。 |
| Task 3：HTML 正文回退与排版 | 子步骤通过 | 用户按参考代码接入按文本子类型递归选择正文及 HTML 回退。导师按委托实现空白折叠、预格式化文本保留、标题和段落换行、表格单元格分隔及 `template` 子树跳过。新增 `tests/mime_test.cpp` 回归用例，先因空白排版不符合预期失败，修改后 macOS Debug 全套 CTest 12/12 通过。 |
| Task 3：嵌套正文选择和 HTML 回退复查 | 子步骤通过 | macOS Debug 全套 CTest 12/12 通过。额外临时虚构样本验证嵌套 `multipart/mixed` 与 `multipart/alternative` 中的纯文本选择和拼接、文本附件排除，以及中文 HTML-only 邮件正文提取。嵌套样本仍需纳入正式验收测试。 |
| Task 3：MIME 与 HTML 解析 | 完成 | 用户完成附件信息提取、MIME 结构限制和字符集检查。导师在 `tests/mime_test.cpp` 补充虚构邮件测试，验证嵌套 multipart 正文的选择与拼接、附件原顺序及中文文件名、媒体类型与解码后的字节数、quoted-printable 中文正文、未知字符集错误以及错误后仍能解析下一封邮件。20 MiB 原文、32 层嵌套及 256 个部件的临界值均可解析，超过限制分别返回 `parse_error`。实际项目 macOS Debug／Release 重新构建后 CTest 各 12/12 通过，存储测试同时验证附件大小超出 SQLite 整数范围时拒绝写入。AC-5 的解析组件验收完成；同步编排中单封坏邮件不阻塞后续邮件的行为由 Task 5 验证。 |
| Task 4：本地 TLS IMAP 测试服务器 | 子步骤通过 | 导师准备 `tests/fixtures/imap_server.py` 和 `imap_fixture` 测试，仅监听本机回环地址的临时端口，生成及清理临时证书。Python 客户端验证 IMAPS、STARTTLS、错误认证、空邮箱、UIDVALIDITY、UID 查询、INTERNALDATE、原文下载及未读状态。额外使用本机 curl 验证两种 TLS 模式下的 UID 查询、`BODY.PEEK[]` 下载及未读状态。macOS Debug／Release 全套 CTest 各 13/13 通过；C++ IMAP 客户端尚未实现。 |
| Task 4：配置 libcurl 连接句柄 | 子步骤通过 | 用户完成 `make_imap_handle`。新增 `imap_connection` 测试，通过 C++ 句柄实际向本机虚构邮箱发起请求；IMAPS、STARTTLS 均验证连接与认证成功、错误密码、密码缺失、无效配置、证书不受信任及主机名不匹配的拒绝行为。macOS Debug／Release 全套 CTest 各 14/14 通过。 |
| Task 4：解析收件箱快照 | 子步骤通过 | 导师按用户委托实现 `parse_mailbox_snapshot`，验证 UIDVALIDITY 和 UID 的非零 32 位整数范围、重复 UIDVALIDITY 一致性、空 SEARCH、缺失和非法响应，以及 UID 升序去重。新增 `imap_snapshot` 测试，重新构建并验证 macOS Debug／Release 全套 CTest 各 15/15 通过（含本机 TLS 测试）。 |
| Task 4：实际读取 INBOX 快照 | 子步骤通过 | 导师按委托完成 `make_imap_handle` 路径配置及 `read_mailbox_snapshot`，用 `UID SEARCH ALL` 查询并解析协议响应。扩充 `imap_connection` 测试，在本机验证 IMAPS 和 STARTTLS 的正常及空邮箱快照、配置和认证失败、证书校验失败、强制 TLS 失败、命令拒绝、非法响应和超时。查询不改变未读标记，不输出协议响应，不包含密码且仅执行一次查询。macOS Debug／Release 构建无警告，全套 CTest 各 15/15 通过。 |
| Task 4：复用 libcurl 选项设置 | 子步骤通过 | 按委托提取文件内 `CurlOptionSetter`，统一句柄选项设置及错误码记录，供连接、快照和后续下载复用。macOS Debug／Release 全套 CTest 各 15/15 通过。 |
| Task 4：下载协议探查 | 准备完成 | 本机 curl 连接虚构 TLS 邮箱，验证在指定 `UIDVALIDITY` 的邮箱路径中执行 UID FETCH 并使用 BODY.PEEK[] 可以取得虚构邮件原文和 INTERNALDATE，下载后未读状态不变；UIDVALIDITY 不匹配时返回错误且不下载原文。此探查不代表 C++ 下载逻辑已实现。 |
| Task 4：解析邮件接收时间 | 子步骤通过 | 按委托实现 `parse_internal_date`，校验固定格式、有效日历日期、时分秒和时区范围，将服务器接收时间换算为 UTC 秒数，保留负时间戳，非法输入返回不包含原始响应的 `protocol_error`。新增 `imap_internal_date` 专项测试，macOS Debug／Release 全套 CTest 各 16/16 通过，构建无警告。 |
| Task 4：单封邮件下载 | 本机子步骤通过 | 按委托实现 `fetch_mail`，下载前校验远端标识和 UIDVALIDITY，通过 `UID FETCH` 与 `BODY.PEEK[]` 收取原文及 INTERNALDATE，按 literal 字节数分离原文与协议字段，保留未读状态。`imap_fetch` 测试覆盖两种 TLS 模式、正常下载、字段位于原文之后、未标记 FLAGS 响应、非文本字节、无末尾换行、空邮箱、邮件消失、UIDVALIDITY 变化或缺失、认证和证书错误、超时、非法及不完整响应，以及 20 MiB 临界值和超限。macOS Homebrew libcurl 8.23.0 下 Debug／Release 全套 CTest 各 17/17 通过，构建无警告。 |
| Task 4：libcurl 最低版本调整 | 子步骤通过 | 对照测试系统 libcurl 8.7.1 时，普通邮件、原文后置字段和无末尾换行样本通过，但 20 MiB 超长行样本不能完整下载。[libcurl 8.17.0 发行说明](https://curl.se/ch/8.17.0.html)记录了自定义 FETCH 字面量处理修复。经用户明确批准，生效的最低版本由原冻结计划中的 8.0 提高至 8.17.0，其余计划不变；CMake 和依赖文档已同步，删除旧版运行时判断和兼容分支。实际 CMake 配置拒绝系统 8.7.1，并接受 Homebrew 8.23.0；macOS Debug／Release 全套 CTest 各 17/17 通过，构建无警告。本轮未在恰好 8.17.0 的运行库上测试。 |
| Task 4：中文注释统一 | 完成 | 按用户要求统一项目源码、测试及构建配置的中文注释，关闭 clang-format 自动生成英文命名空间注释的功能，并将这一规范写入项目约定。 |
| Task 4：有限重试与断连恢复 | 子步骤通过 | 按委托实现收件箱快照和单封下载的统一重试：只对临时连接、传输和明确的服务不可用故障重试，最多两次，分别等待 1、2 秒。各次尝试重新创建连接句柄和响应缓冲区，下载重连时重新核对 UIDVALIDITY。新增 `imap_retry` 本地 TLS 测试，验证连接中断、查询中断、原文下载中断、超时和服务暂不可用后的恢复，重试耗尽、退避时间、重连后邮件消失或 UIDVALIDITY 变化立即停止，以及完整原文和未读状态。认证、证书、参数、协议错误与下载超限直接报告，不在当前调用中反复请求。 |
| Task 4：IMAP 接入 | 完成 | AC-6 本机验收通过。使用 Homebrew libcurl 8.23.0 与虚构 TLS 邮箱实际验证 IMAPS、STARTTLS、快照读取、正常下载、空邮箱、认证、证书、超时、断连恢复、有限重试、邮件消失、UIDVALIDITY 变化、INTERNALDATE、原文大小边界、未读状态保持及错误信息脱敏。macOS Debug／Release 全套 CTest 各 18/18 通过，构建无警告。真实邮箱验证留待 Task 7。 |
| Task 4：IMAP 组件结构整理 | 完成 | 根据代码审查建议，将纯协议解析移入 `imap_protocol.cpp`／`.h`，网络传输留在 `imap.cpp`，按公共工具、快照、下载、公开入口聚合，统一文件内匿名命名空间和错误别名。`FetchTransfer` 封装协议状态，新增 `imap_fetch_protocol` 内存测试，覆盖属性顺序、原文分片与重组、完整性、大小限制、错误优先级及远端标识。单独编译协议组件及其测试时不链接 libcurl 也能通过。原公开接口、重试和下载行为不变；macOS Debug／Release 全套 CTest 各 19/19 通过，构建无警告，独立只读审查未发现阻塞问题。 |
| Task 5：同步编排与恢复 | 未验收 | 尚无本阶段整体同步通过记录。 |
| Task 6：缓存与 CLI 接入 | 未验收 | 终端仍使用阶段 1 示例数据。 |
| Task 7：真实邮箱与阶段验收 | 未完成 | 真实邮箱与完整阶段回归仍需执行。 |

当前接续位置：Task 5，完成增量同步，并在失败或重启后继续。Task 2 已完成组件实现与存储验收，写命令入口的资源装配由 Task 6 接入；Task 3 已完成解析组件验收；Task 4 已完成 IMAP 本机验收。后续状态及证据仅更新本文件，正式计划保持不变。

新库使用最初的版本 1 账号表定义。使用提交 `d0c9410` 中另一种缩进创建的数据库也可以打开，原有结构和数据保留。详见[生命周期设计](../../specs/mail-ai-assistant-sqlite-lifecycle-design.md)第 4.1 节。

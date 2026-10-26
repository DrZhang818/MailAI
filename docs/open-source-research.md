# 邮件 AI 助手：开源调研记录

调研日期：2026-10-19。用途：为 [Superpowers 设计规格](superpowers/specs/mail-ai-assistant-design.md) 提供产品与工程参考。旧需求、架构与阶段草案已合并到该规格，本文保留实际查阅的来源与观察。

本轮阅读一手项目 README、官方架构文档和可访问的关键源码，没有部署运行这些项目；下面区分原项目已描述／实现的做法与本项目拟采用的取舍。链接使用当时的 main／master 页面，之后可能变化。

## 1. Mailspring / Mailspring-Sync：C++ 邮件核心与账号隔离

项目入口：[Mailspring](https://github.com/Foundry376/Mailspring)、[Mailspring-Sync README](https://github.com/Foundry376/Mailspring-Sync#core-concepts)。

文档说明：界面与本地 C++ 同步引擎分离；同步引擎基于 MailCore2，使用 SQLite。每个连接账号对应一个同步进程，界面与引擎通过 JSON 数据通信；任务持久化，并区分本地更新与远端网络操作。这为多账号的职责隔离提供了具体参考。

实际查看的关键源码：[TaskProcessor.cpp](https://github.com/Foundry376/Mailspring-Sync/blob/master/MailSync/TaskProcessor.cpp)，尤其 performRemoteSendDraft。该函数先持久化执行标志，避免同一发送任务卡在队列里不断发送；代码说明重试需要生成新任务。这个机制不等于跨网络的恰好一次投递保证。

本项目拟采用：邮件核心可脱离界面测试，账号会话独立，发信有持久化任务和明确状态。当前先用简单的任务调度与账号隔离，不复制其多进程和完整同步框架；发送结果未知时单独处理。

## 2. Thunderbird：账号、收信服务器、发件身份和发信服务器

阅读入口：[Accounts, Servers and Identities](https://source-docs.thunderbird.net/en/latest/backend/accounts.html)。

其模型把账号与收信配置、发件身份、发信配置分开。账号关联收信服务器和身份；身份保存发件地址等信息，并关联发信服务器。成熟客户端可以在同一账号下拥有多个身份。

本项目拟采用：首版一个账号一个身份，但在数据上区分“从哪里收”和“以谁的身份发”。邮件和回复任务始终关联来源账号，统一收件箱只改变展示方式。这样 QQ 和学校邮件混合查看时，仍可以确定每条回复该使用哪个账号。

## 3. Inbox Zero：会话判断、生成草稿与邮件服务适配

项目入口：[Inbox Zero](https://github.com/elie222/inbox-zero)。其文档描述了邮件整理、AI 规则、预生成回复、回复状态跟踪和多账号邮件界面。项目重点接入 Gmail／Outlook，不能据此宣称它已经满足 QQ／学校邮箱需求。

本轮查看的具体实现：

| 源码入口 | 实际观察 | 对本项目的启发 |
|---|---|---|
| [provider.ts](https://github.com/elie222/inbox-zero/blob/main/apps/web/utils/email/provider.ts) | 按账号与服务商创建 Gmail／Outlook 实现，并记录服务商错误 | 分析逻辑不应依赖某个邮箱的网络细节；首版先做 IMAP／SMTP，OAuth 单独扩展 |
| [determine-thread-status.ts](https://github.com/elie222/inbox-zero/blob/main/apps/web/utils/ai/reply/determine-thread-status.ts) | 读取会话上下文，返回状态及理由；区分待回复、等待对方、仅供知悉和已处理，并校验结构化输出 | “重要性”与“是否需要回复”分开；已回复记录和未解决问题都会影响判断 |
| [draft-reply.ts](https://github.com/elie222/inbox-zero/blob/main/apps/web/utils/ai/reply/draft-reply.ts) | 使用会话与上下文生成回复，约束事实来源、语言与格式，返回结构化结果后再整理文本 | 生成回复时提供必要上下文，不让模型凭空补个人决定或已完成事项 |
| [email/types.ts](https://github.com/elie222/inbox-zero/blob/main/apps/web/utils/email/types.ts) | 分开定义 createDraft、sendDraft、sendEmail 等能力，草稿接口保留回复关联 | 草稿与发送分开，可同时支持审核发送和选中后直接生成发送 |

本项目拟采用上述职责划分。用户要求的一键回复是本产品明确增加的交互：选择本次邮件集合后直接生成并发送，不因参考项目有草稿流程就改成逐封审批。本轮未验证 Inbox Zero 的每个界面或自动化规则是否具有完全相同的批量行为。

## 4. 对需求的具体影响

1. 多账号不是把账号密码放进一个数组就结束：邮件、同步进度、错误与发送身份都需要有账号归属。
2. 排序、隐藏、是否需要回复是不同维度，采用结构化分析与可解释理由，显示模式由程序控制。
3. 用户规则应能覆盖 AI 判断；低价值邮件先在本地隐藏，保留恢复入口。
4. 草稿生成与网络发送分开，普通模式停在草稿，一键模式继续发送。
5. 发送状态必须落盘，避免重复点击或重启造成重复投递；不把超时直接当作未发送。
6. 先完成可测试的 C++ 核心，再增加界面。真实适配 QQ／学校邮箱需要自己的协议连接与兼容性测试。

这些是调研后的设计取舍，不宣称所有参考项目都以相同方式实现。

## 5. 邮箱与协议资料

- [北交大客户端专用密码教程](https://ic.bjtu.edu.cn/fwzn/xxxtzn/202311421.htm)：确认本人账号的客户端认证与页面参数；连接能力仍待实测。
- [Gmail XOAUTH2](https://developers.google.com/workspace/gmail/imap/xoauth2-protocol)、[Microsoft IMAP／SMTP OAuth](https://learn.microsoft.com/en-us/exchange/client-developer/legacy-protocols/how-to-authenticate-an-imap-pop-smtp-application-by-using-oauth)：作为后续服务商适配依据。
- [RFC 9051](https://www.rfc-editor.org/rfc/rfc9051.html)：IMAP 标识、只读访问和状态语义；已在前期计划中核对。
- [RFC 3834](https://www.rfc-editor.org/rfc/rfc3834.html)：涉及无人值守自动响应时，应单独考虑自动邮件标识和邮件循环。当前需求是一键处理用户选中的集合，不自动扩展为对所有新邮件持续回复。

QQ 帮助页本轮未取得可核实的完整客户端配置正文，因此需求不硬编码服务器或端口。实施时以本人 QQ 邮箱设置页和可访问的官方帮助为准，实际验证收信与发信两条连接。

# Team：一个终端里的 GPT 与 DeepSeek 自然接力

在项目目录运行 `team`，直接输入消息。GPT 和 DeepSeek 使用各自的真实 Codex 会话，像普通 Codex 一样回答、讨论或操作项目；它们能看到彼此公开发言，并通过隐藏的发言权标记决定下一棒交给谁。

```text
你 › 能收到我的消息吗

GPT · 发言
  能收到。

DeepSeek · 发言
  也能收到，你直接说任务就行。

GPT · 发言
  收到，等你发下一步。
```

默认顺序是 GPT → DeepSeek → GPT → 用户，但没有固定讨论轮数。模型可以持续接力，也可以提前结束或交还用户。

## 隐藏交接标记

成员只在需要决定下一棒时，在回复最后单独写：

```text
[[next:gpt]]
[[next:deepseek]]
[[next:user]]
[[end]]
```

标记不会显示给用户。没有标记时，终端使用默认顺序；因此普通用户消息不会被改写成“当前任务、阶段、唯一执行者”之类的模板。

## 安装与运行

要求 WSL/Linux、Python 3.11+、Codex CLI（本机 0.164.0），以及现有 GPT 登录和 `deepseek-menu` 配置。不安装额外 Python 依赖。

```bash
python3 tools/codex-team/install.py
team --doctor
team --check-connection
team
```

也可直接运行：

```bash
python3 tools/codex-team/cli.py
```

```bash
team -C /path/to/project
team '检查当前配置读取逻辑'
team --read-only '只分析当前问题，不修改文件'
```

带任务参数运行一次后退出；不带参数进入交互界面。管道输入作为一条用户消息读取，空管道不会启动模型。

## 交互

- 直接输入普通消息或开发任务；不会自动触发“分工协商”阶段。
- 执行中继续输入会优先作为用户插话送达当前发言人，不会另开第二条任务。
- `/status` 查看当前发言人和连接状态。
- `/stop` 或 Ctrl+C 停止当前接力。
- `/verbose` 显示或隐藏工具命令。默认隐藏命令，只显示发言、文件修改摘要和错误。
- `/allow N` 或 `/deny N` 批准/拒绝一次需要确认的操作。
- `/help` 显示帮助，`/quit` 或 Ctrl+D 退出。

同一时间只有一个成员活跃，因此工具调用和文件写入串行进行。成员可以自行完成任务，也可以把任务或检查交给对方。

## 上下文与提示

- 第一条用户消息原样进入 GPT 的 Codex 回合。
- 后续只有未读的对方发言会以 `[GPT]`、`[DeepSeek]` 或 `[你]` 标签传给当前模型。
- 每个模型只在线程启动时收到一次最小房间说明：自己的名字、组员名字、用户在场和隐藏交接格式。
- 不再发送 JSON 计划、阶段指令、固定角色或任务所有权提示。

## 配置与记录

GPT 复用 `~/.codex/config.toml` 与现有登录；DeepSeek 复用 `~/.codex/deepseek-menu.config.toml`。密钥依次读取 `DEEPSEEK_API_KEY`、已有菜单的 `~/.local/share/codex-menu/credentials.json`，仅通过子进程环境传入。原有 `codex` 命令与配置不变。

公开会话记录默认保存在 `~/.local/share/codex-team/sessions/`，文件权限 0600；不会记录配置中的凭据。记录可能包含任务和代码内容，请按项目数据保存。

正常讨论没有固定轮数上限。为避免两个模型无限重复同一段话，连续重复的接力会停止并交还用户；用户也可以随时 `/stop`。工具不会自动提交、重置或清理用户已有修改。

## 验证与卸载

```bash
python3 -m unittest discover -s tools/codex-team/tests -v
python3 ~/.local/share/codex-team/install.py --uninstall
```

卸载只移除 `team` 入口，保留程序和会话记录。重复安装更新程序，保留既有配置和会话；若 `team` 已由其他工具占用，安装器拒绝覆盖。

实现基于 [Codex App Server](https://learn.chatgpt.com/docs/app-server)，协议字段以本机 CLI 生成的 schema 为准。

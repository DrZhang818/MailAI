# Mail AI Assistant

使用 C++23 开发的终端邮件 AI 助手。

当前完成阶段 1：使用内存中的虚构邮件运行终端程序。

- 列出全部邮件或按账号筛选。
- 按接收时间降序、同时间 ID 升序排列。
- 按稳定邮件 ID 查找详情。
- 区分空结果、未知账号和无效邮件 ID。
- 提供命令行帮助、参数校验和退出码。

成功展示邮件时标明“数据来源：示例”。示例账号为 `school` 和 `qq`，不代表已连接真实邮箱；真实收信、持久化、AI 和回复功能将在后续阶段接入。

## 构建与测试

需要 WSL Ubuntu、支持 C++23 的 GCC 及提供 `std::expected` 的标准库、CMake 3.20+ 和 Ninja。

核心接口统一使用 `Result<T> = std::expected<T, Error>` 返回业务结果；不返回数据的操作使用 `Status = Result<void>`。

从 Windows 终端执行：

```powershell
wsl.exe -d Ubuntu -- cmake -S /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant -B /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug

wsl.exe -d Ubuntu -- cmake --build /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug

wsl.exe -d Ubuntu -- ctest --test-dir /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug --output-on-failure
```

CTest 包含 `inbox`（核心查询）、`cli`（参数、展示与错误处理）、`config`（阶段 2 的环境变量配置解析）和 `cli_smoke`（实际程序入口）。测试使用独立的条件检查，Release 模式下也会执行。

## 使用

```powershell
wsl.exe -d Ubuntu -- /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug/mail_assistant --help

wsl.exe -d Ubuntu -- /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug/mail_assistant inbox

wsl.exe -d Ubuntu -- /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug/mail_assistant inbox --account school

wsl.exe -d Ubuntu -- /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/debug/mail_assistant show 43
```

`inbox --account all` 与 `inbox` 含义相同。`show` 使用邮件自身的 ID，不使用列表行号。ID 为十进制正整数，最大值为 `18446744073709551615`；ID 合法但查无邮件时返回运行失败。

正常结果写 stdout，错误原因写 stderr。

`--account` 需要非空值；缺少值或用另一选项代替值时，按参数错误处理。格式正确但不存在的账号仍属于查询失败。

| 退出码 | 含义 | 示例 |
|---|---|---|
| 0 | 成功，包括帮助和空列表 | `inbox`、`show 43` |
| 1 | 查询失败 | `inbox --account missing`、`show 999` |
| 2 | 参数错误 | `show 42x`、`show 0`、`inbox --account` |

## Release 验证

```powershell
wsl.exe -d Ubuntu -- cmake -S /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant -B /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/verify-stage1-release -G Ninja -DCMAKE_BUILD_TYPE=Release

wsl.exe -d Ubuntu -- cmake --build /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/verify-stage1-release

wsl.exe -d Ubuntu -- ctest --test-dir /mnt/d/programme/gitcode/实习准备/projects/mail-ai-assistant/build/verify-stage1-release --output-on-failure
```

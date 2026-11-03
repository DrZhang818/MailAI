# Mail AI Assistant

使用 C++23 开发的终端邮件 AI 助手。

当前完成阶段 1：使用内存中的虚构邮件运行终端程序。

阶段 2 已实现环境变量配置、共享类型、基础依赖及 SQLite 数据库生命周期：五表版本 1 初始化、结构校验和失败回滚。终端仍使用示例数据；邮件缓存业务读写和真实收信将在后续任务实现。

- 列出全部邮件或按账号筛选。
- 按接收时间降序、同时间 ID 升序排列。
- 按稳定邮件 ID 查找详情。
- 区分空结果、未知账号和无效邮件 ID。
- 提供命令行帮助、参数校验和退出码。

成功展示邮件时标明“数据来源：示例”。示例账号为 `school` 和 `qq`，不代表已连接真实邮箱；真实收信、持久化、AI 和回复功能将在后续阶段接入。

## 构建与测试

需要 macOS、支持 C++23 和 `std::expected` 的编译器及标准库、CMake 3.20+、Ninja，以及 SQLite、libcurl、GMime、libxml2 和 pkg-config。

核心接口统一使用 `Result<T> = std::expected<T, Error>` 返回业务结果；不返回数据的操作使用 `Status = Result<void>`。

库的最低版本和实际验证结果见[依赖记录](docs/dependencies.md)。

使用 Homebrew 安装依赖（CMake 和 Apple 命令行开发工具须已安装）：

```sh
brew install ninja pkgconf sqlite curl gmime libxml2 libffi
```

在项目根目录的 macOS 终端执行：

```sh
cmake -S . -B build/macos-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$(sw_vers -productVersion)" \
  "-DCMAKE_PREFIX_PATH=$(brew --prefix libffi);$(brew --prefix sqlite);$(brew --prefix curl);$(brew --prefix libxml2);$(brew --prefix)"
cmake --build build/macos-debug
ctest --test-dir build/macos-debug --output-on-failure
```

命令将最低运行系统版本设为当前 macOS 版本，并显式选用 Homebrew 的库；生成的程序不保证可在较旧的 macOS 上运行。

CTest 包含 `inbox`（核心查询）、`cli`（参数、展示与错误处理）、`config`（环境变量配置解析）、`store`（数据库生命周期、约束、恢复及账号配置）和 `cli_smoke`（实际程序入口）。测试使用独立的条件检查，Release 模式下也会执行。

数据库生命周期由核心库的 `SqliteStore::open` 提供，尚未接入 CLI。读写打开仅在空版本 0 上初始化完整版本 1，并启用外键和文件数据库 WAL；只读打开仅接受结构匹配的版本 1。非空版本 0 和其他不支持的版本均返回错误，保留原结构及数据。结构校验只兼容本项目已知的 DDL，手工改写的等价 SQL 也可能被拒绝；具体契约见[生命周期设计](docs/superpowers/specs/mail-ai-assistant-sqlite-lifecycle-design.md)。

账号建表 SQL 的缩进曾调整；此前按旧缩进创建的版本 1 数据库会被拒绝打开，原结构和数据保留。

## 使用

```sh
./build/macos-debug/mail_assistant --help
./build/macos-debug/mail_assistant inbox
./build/macos-debug/mail_assistant inbox --account school
./build/macos-debug/mail_assistant show 43
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

```sh
cmake -S . -B build/verify-stage1-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$(sw_vers -productVersion)" \
  "-DCMAKE_PREFIX_PATH=$(brew --prefix libffi);$(brew --prefix sqlite);$(brew --prefix curl);$(brew --prefix libxml2);$(brew --prefix)"
cmake --build build/verify-stage1-release
ctest --test-dir build/verify-stage1-release --output-on-failure
```

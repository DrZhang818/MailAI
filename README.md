# MailAI

MailAI is a C++23 command-line mail assistant in development. Its current
executable demonstrates browsing a small, in-memory set of fictional emails.
The core library also contains SQLite storage and synchronization bookkeeping
that are not yet connected to the command line.

## Current status

The CLI can:

- List sample messages from all accounts or filter them by account.
- Sort messages by receive time, newest first, with the mail ID as a tie-breaker.
- Show a sample message by its ID.
- Report invalid arguments, unknown accounts, and missing messages.

The sample accounts, `school` and `qq`, do not connect to real mailboxes. The
CLI labels successful message output as sample data and currently prints its
message content and most diagnostics in Chinese.

The core library provides environment-based configuration parsing and a SQLite
store for accounts, messages, attachments, and synchronization state. The store
can register pending messages, save fetched messages and parsing results, record
failures and synchronization outcomes, and handle UIDVALIDITY changes. These
are library capabilities, not CLI commands. Real IMAP fetching, MIME parsing,
CLI access to the local store, AI features, and replies are not yet implemented.
A data-directory write lock is also still pending.

## Build from source

You need:

- A C++23 compiler and standard library with `std::expected` support.
- CMake 3.20 or later, Ninja, and pkg-config.
- SQLite 3.37 or later, libcurl 8.0 or later, GMime 3.2 or later, and
  libxml2 2.9 or later.

With Git installed, clone the repository first:

```sh
git clone https://github.com/DrZhang818/MailAI.git
cd MailAI
```

### macOS

The commands below use Homebrew and the Apple command-line developer tools.

Install CMake if it is not already available, then install the remaining tools
and libraries:

```sh
brew install ninja pkgconf sqlite curl gmime libxml2 libffi
```

Configure and build:

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$(sw_vers -productVersion)" \
  "-DCMAKE_PREFIX_PATH=$(brew --prefix libffi);$(brew --prefix sqlite);$(brew --prefix curl);$(brew --prefix libxml2);$(brew --prefix)"
cmake --build build/debug
```

This configuration selects Homebrew libraries and sets the deployment target to
the current macOS version; compatibility with older versions is not guaranteed.

### Linux (Ubuntu 24.04)

Install the compiler, build tools, and development libraries. Enable the
Ubuntu `universe` repository if `libgmime-3.0-dev` is not available.

```sh
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config \
  libsqlite3-dev libcurl4-openssl-dev libgmime-3.0-dev libxml2-dev
```

Configure and build from the repository root:

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
```

An earlier version of the project was built and tested on WSL Ubuntu, but the
Ubuntu 24.04 instructions above have not yet been tested with the current code.
Other distributions need equivalent packages that meet the minimum versions.
See [dependency notes](docs/dependencies.md) for
previously tested versions and platform details.

## Try the CLI

```sh
./build/debug/mail_assistant --help
./build/debug/mail_assistant inbox
./build/debug/mail_assistant inbox --account school
./build/debug/mail_assistant show 43
```

`inbox --account all` is equivalent to `inbox`. The `show` argument is the
message ID, not a row number. It must be a positive decimal integer in the
unsigned 64-bit range. Successful output goes to stdout; errors go to stderr.

| Exit code | Meaning |
| --- | --- |
| 0 | Success, including help or an empty result |
| 1 | Query or execution failure |
| 2 | Invalid command-line arguments |

## Run the tests

```sh
ctest --test-dir build/debug --output-on-failure
```

CTest covers the in-memory inbox and CLI, configuration parsing, SQLite
lifecycle and queries, synchronization bookkeeping, message writes, and a CLI
smoke test.

## Storage notes

`SqliteStore` is available through the core library but is not used by the
current executable. Its schema validation accepts the project's known version 1
DDL; manually rewritten equivalent SQL may be rejected. Databases created by
commit `d0c9410` use different account-table formatting and are currently
rejected without changing their structure or data. See the
[SQLite lifecycle design](docs/superpowers/specs/mail-ai-assistant-sqlite-lifecycle-design.md)
for the compatibility contract.

## Project documentation

- [Project design](docs/superpowers/specs/mail-ai-assistant-design.md)
- [Foundation plan](docs/superpowers/plans/phase-01-foundation/plan.md) and
  [progress](docs/superpowers/plans/phase-01-foundation/progress.md)
- [Mail synchronization plan](docs/superpowers/plans/phase-02-mail-sync/plan.md)
  and [progress](docs/superpowers/plans/phase-02-mail-sync/progress.md)

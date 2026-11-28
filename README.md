# MailAI

MailAI is a C++23 command-line mail assistant in development. It supports
incremental IMAP synchronization, MIME parsing, SQLite storage, and offline
mail browsing. A separate demo mode provides fictional emails for exploration.

## Current status

The CLI can:

- Sign in interactively on macOS and remember mailbox settings and the cache
  location across terminal sessions. Save the password in macOS Keychain.
- Synchronize the configured INBOX into the local SQLite cache.
- List cached messages or filter them by account without connecting to IMAP.
- Display messages by ascending stable local ID and search cached sender,
  subject, and body text.
- Show a message by its stable local ID, including attachments and parsing status.
- Browse the original fictional messages using `--demo`.
- Report invalid arguments, unknown accounts, and missing messages.

The sample accounts, `school` and `qq`, do not connect to real mailboxes. The
CLI labels output with its data source, displays mail content in its original
language, and reports errors in English.

The core library provides environment-based configuration parsing and a SQLite
store for accounts, messages, attachments, and synchronization state. The store
can register pending messages, save fetched messages and parsing results, record
failures and synchronization outcomes, and handle UIDVALIDITY changes. These
functions are coordinated by the `sync_inbox` service, which reads the INBOX
snapshot, schedules the newest 20 messages on the first synchronization or
after a UIDVALIDITY change, and processes new and pending messages in UID order.
Successful messages are not downloaded again; failures can be retried and
unfinished work survives a restart. The IMAP client uses read-only fetching.
Explicit history backfill can schedule a limited number of older uncached
messages or all remaining messages in the current INBOX.
A move-only data-directory write lock protects synchronization, with bounded
waits and automatic release. Initial real-mailbox receipt has been verified;
final live acceptance, AI features, and replies are still planned.

## Build from source

You need:

- A C++23 compiler and standard library with `std::expected` support.
- CMake 3.20 or later, Ninja, and pkg-config.
- SQLite 3.37 or later, libcurl 8.17.0 or later, GMime 3.2 or later, and
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
brew install ninja pkgconf sqlite curl gmime libxml2 libffi python openssl
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

The VS Code workspace disables automatic formatting on save, paste, and typing.
Manual C++ formatting uses `clang-format 23+` and the root `.clang-format`;
it may change hand-formatted code.

### Linux (Ubuntu 24.04)

Install the compiler, build tools, and development libraries. Enable the
Ubuntu `universe` repository if `libgmime-3.0-dev` is not available.

```sh
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config \
  libsqlite3-dev libcurl4-openssl-dev libgmime-3.0-dev libxml2-dev python3 openssl
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
./build/debug/mail_assistant --demo inbox
./build/debug/mail_assistant --demo show 43
./build/debug/mail_assistant --demo find "面试"
```

By default, `inbox`, `find`, and `show` read the local cache without requiring a password
or a network connection. An absent cache is an empty inbox, not sample data.
Use an ID displayed by `inbox` with `show`. Demo mode cannot run `sync`.

The inbox is rendered as a column-aligned terminal table, including Chinese
text. Long subjects wrap across lines without being truncated; sender
addresses may be shortened when space is limited. Use `show` for the
complete mail and timestamp. HTML body tables are recovered from the locally
stored original message and rendered as text tables. Images are not drawn or
downloaded from remote URLs; embedded image data remains in the original MIME.
Interactive terminals use colored headers and
dividers; set `NO_COLOR` to disable color. Historical mail is labeled only
when it belongs to a retired UIDVALIDITY range; it remains available locally.

### Interactive login on macOS

```sh
./build/debug/mail_assistant login
./build/debug/mail_assistant sync
./build/debug/mail_assistant inbox
```

Enter the mailbox address and the credential requested for your provider (such as
a client-specific password or IMAP authorization code). Common providers
(including BJTU, QQ, 163, 126, yeah.net, Gmail, Outlook, and iCloud)
automatically use their standard IMAP host, implicit TLS on port 993, the
mailbox address as username, and the default cache directory; unknown domains
prompt for the IMAP host. Run `./build/debug/mail_assistant login --advanced` to
customize the IMAP host, username, TLS mode, port, or cache directory (for an
existing cache, choose its existing data directory).
The password prompt does not echo the input. Login verifies IMAP access over
TLS before saving settings; it does not download mail or change read status.

New mail cache directories are created with owner-only permissions (0700).
Before writing, `sync` protects `cache.sqlite3` and any existing SQLite
journal, WAL and shared-memory files with mode 0600. An existing directory is
not automatically chmodded: it must be owned by the current user and not be
writable by other users. Existing caches receive file-permission protection
the next time `sync` runs; read-only commands do not change permissions.

The profile is saved in `$XDG_CONFIG_HOME/mail-ai-assistant/login.conf` when
the XDG path is absolute, otherwise in
`$HOME/.config/mail-ai-assistant/login.conf`. Only nonsecret settings are
saved there (directory mode 0700, file mode 0600). The password is stored
in macOS Keychain using the Security framework, not in this file, the
database, or the shell command line. macOS may ask for permission to use
the saved credential. Restarting the terminal does not require another login.

```sh
./build/debug/mail_assistant logout
```

Logout removes the saved Keychain credential but retains nonsecret mailbox
settings and all cached mail. Read-only commands continue to work offline.
Run `login` again to resume synchronization. Linux interactive login is
not implemented yet.

### Explicit environment configuration

Environment-based configuration remains available for scripts and tests.
If any of the three mailbox identity variables below is explicitly set,
`sync` requires the complete environment configuration instead of combining
it with remembered credentials. Use a fresh terminal without those variables
to use the remembered login. An explicit `MAILAI_DATA_DIR` overrides the
remembered cache path. Logout does not remove credentials supplied separately
by environment variables.

To configure synchronization manually, set the following environment variables:

| Variable | Meaning |
| --- | --- |
| `MAILAI_EMAIL`, `MAILAI_IMAP_HOST`, `MAILAI_IMAP_USER` | Required mailbox address, IMAP host, and username |
| `MAILAI_ACCOUNT_ID`, `MAILAI_ACCOUNT_NAME` | Optional local account ID and display name; default to `school` and `学校邮箱` |
| `MAILAI_IMAP_TLS`, `MAILAI_IMAP_PORT` | Optional TLS mode (`implicit` or `starttls`) and port; default to implicit TLS on 993, or 143 for STARTTLS |
| `MAILAI_PASSWORD_ENV` | Name of the environment variable containing the password; default `MAILAI_IMAP_PASSWORD` |
| `MAILAI_IMAP_CA_FILE` | Optional CA certificate file for a server using a custom CA; certificate and hostname verification remain enabled |
| `MAILAI_DATA_DIR` | Optional data directory override |

Keep the password in the referenced environment variable, not in the
configuration, shell history, or repository. The data directory is selected
from `MAILAI_DATA_DIR`, an absolute `XDG_DATA_HOME` with the
`mail-ai-assistant` suffix, or `$HOME/.local/share/mail-ai-assistant`.
The database is named `cache.sqlite3`.

After configuring your own mailbox, use:

```sh
./build/debug/mail_assistant sync
./build/debug/mail_assistant sync --history 50
./build/debug/mail_assistant sync --all
./build/debug/mail_assistant inbox
./build/debug/mail_assistant find "课程"
```

Ordinary `sync` initially downloads the newest 20 messages, then processes
new messages and retries unfinished work. Use `--history N` to also backfill
up to N more recent uncached older messages, or `--all` to backfill the
entire current INBOX. Already cached messages and pending jobs do not consume
the history quota. These explicit options may download large amounts of mail.
`find` performs literal substring matching across cached sender, subject,
and body (ASCII letters are case-insensitive); it does not search the server.

`inbox --account all` is equivalent to `inbox`. The `show` argument is the
message ID, not a row number. It must be a positive decimal integer in the
unsigned 64-bit range. Successful output goes to stdout; errors go to stderr.

| Exit code | Meaning |
| --- | --- |
| 0 | Success, including help or an empty result |
| 1 | Query or execution failure |
| 2 | Invalid command-line arguments |
| 3 | Partial synchronization success |

## Verify a live mailbox

For the first real connection, build the opt-in probe and run it with your
configured mailbox:

```sh
cmake --build build/debug --target imap_live_probe
MAILAI_LIVE_TESTS=1 ./build/debug/imap_live_probe
```

The probe checks TLS, authentication, the INBOX snapshot, and at most the five
newest messages. It prints only counts and diagnostics, does not open the
SQLite cache, and does not change the server's read flags. It is excluded from
the default build and CTest; running it without the explicit opt-in does not
connect to the mailbox. Use a client-specific password provided by your mail
service, enter it locally without echoing it, and never put it in a command's
arguments or a tracked file.

After a successful probe, choose a private data directory outside the
repository and run `sync`, `inbox`, and `show <id>`. Repeat `sync` and verify
the original local IDs and record count remain stable when no new mail has
arrived. Remove the password from the environment, disconnect the network, and
repeat the cache queries. Check read flags in the webmail client before and
after downloading. Record only counts and pass/fail outcomes, not personal
email addresses, subjects, bodies, attachment names or credentials.

## Run the tests

```sh
ctest --test-dir build/debug --output-on-failure
```

CTest covers the in-memory inbox and CLI, configuration parsing, SQLite
lifecycle and queries, synchronization bookkeeping, message writes, and a CLI
smoke test.

The `sqlite_context` test also covers SQL parameter binding, statement reuse,
and error handling. The `data_dir_lock` test covers process contention,
timeouts, ownership transfer, release, and data preservation. The `mime` test
checks synthetic messages and parsing limits. The `imap_fixture` test uses
Python 3.8+ and the `openssl` command to generate temporary certificates and
start a loopback-only TLS IMAP server with synthetic messages; it never connects
to a real mailbox. The `imap_connection` test uses the C++ libcurl handle to
check both TLS modes, authentication and certificate validation. The
`imap_snapshot` test checks the parsed UIDVALIDITY and UID list. The
`imap_internal_date` and `imap_fetch` tests verify reception timestamps, exact
message bytes, read-only fetching, remote identity checks, transfer errors and
the 20 MiB download limit. The `imap_fetch_protocol` test exercises literal
reassembly, attribute ordering, incomplete responses, and UIDVALIDITY updates
after the literal without a network connection. The `imap_retry` test checks
connection reuse across FETCH requests and continued use of the selected
UIDVALIDITY after unsolicited `EXISTS` updates. It also checks that new
connections must report the expected UIDVALIDITY, including when libcurl
reopens an idle connection and when the application retries a transient
failure. Application retries use delays of one and two seconds. The `sync`
test combines a controlled mail source with real SQLite
storage and MIME parsing to check incremental discovery, failure recovery,
interruption and restart, UIDVALIDITY changes, and storage failures.
The `cli_integration` test runs the actual executable against a loopback TLS
fixture and temporary cache, checking synchronization, offline browsing,
partial success, duplicate-free recovery, write-lock contention, and CLI
validation without connecting to a real mailbox. On macOS it also drives
the login command in a pseudo-terminal to verify that the password is
not echoed and invalid authentication does not save credentials.
The `login_profile`, `login_service`, and `credential_store` tests verify
private profile persistence, credential rollback, retained cache selection,
account isolation and Keychain API behavior with simulated system calls.
On macOS with Homebrew libcurl 8.23.0, Debug and Release each passed all
24 tests. libcurl 8.17.0 is required for custom FETCH
literal handling; older versions are not supported.
This version has not been tested on Linux.

## Source layout

The executable entry point is `src/main.cpp`, while implementation files are
grouped by responsibility:

- `src/cli/`: command-line arguments, login prompts, and terminal output.
- `src/app/`: business services and synchronization orchestration.
- `src/config/`: runtime configuration, remembered login profiles, and Keychain access.
- `src/mail/` contains IMAP transport and protocol parsing, MIME parsing, and
  HTML text and table extraction in `html_extract.{h,cpp}`.
- `src/store/`: SQLite persistence and data-directory locking.
- `src/util/` contains shared file descriptor and directory operations.

Public core-library headers remain in `include/mailai/`. Internal headers
stay beside their implementations.

## Storage notes

Storage operations share `SqlContext` for parameter binding, query execution,
and error context. Its `query_one` and `query_all` operations use named decoders
to turn SQL rows into owned C++ values. `SqlRow` checks column types, NULLs,
integer ranges, and booleans before copying those values. Business mappings
live in [`sqlite_rows.cpp`](src/store/sqlite_rows.cpp).

The SQL interface is declared in `sqlite_utils.h`, template definitions live
in `sqlite_utils.hpp`, and ordinary functions are implemented in
`sqlite_utils.cpp`. Single statements require a value for each parameter slot.
Pass `nullptr` to bind SQL NULL. `execute_many` takes common parameters and per-row
parameters, prepares once, and reuses the statement for the batch. Each row
supplies all remaining parameters. The caller owns the surrounding transaction.
The statement handle and its preparation, binding, and stepping helpers are
private. The private `_prepare` operation prepares SQL and binds the initial
parameters together. Schema initialization uses the shared SQL context.

The public store methods coordinate transactions and business steps. Message
completion saves the message, replaces its attachments, and updates its pending
job within one transaction. Discovery similarly groups mailbox-range changes,
the discovery cursor, and new jobs. Shared sync-state reads use the calling
operation's error context.

`SqliteStore` is used by the runtime services for local cache queries and
synchronization. Its schema validation accepts the project's known version 1
DDL, including the account-table formatting shipped in commit `d0c9410`.
Both layouts open with their existing schema and data. Other SQL formatting
may be rejected. See the
[SQLite lifecycle design](docs/superpowers/specs/mail-ai-assistant-sqlite-lifecycle-design.md)
for the compatibility contract.

## Project documentation

- [Project design](docs/superpowers/specs/mail-ai-assistant-design.md)
- [Foundation plan](docs/superpowers/plans/phase-01-foundation/plan.md) and
  [progress](docs/superpowers/plans/phase-01-foundation/progress.md)
- [Mail synchronization plan](docs/superpowers/plans/phase-02-mail-sync/plan.md)
  and [progress](docs/superpowers/plans/phase-02-mail-sync/progress.md)

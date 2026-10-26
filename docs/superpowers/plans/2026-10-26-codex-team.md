# Codex Team Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide `team`: one terminal, two real models, dynamic task allocation, optional independent review, and no duplicate ownership.

**Architecture:** Two Codex app-server processes reuse existing configuration. A runtime broadcasts public messages, obtains a mutually confirmed task graph, and schedules uniquely owned subtasks with optional review. Independent scopes run concurrently, overlapping writes queue, and failed tasks may be renegotiated without repeating completed work.

**Tech Stack:** Python 3.11+ standard library; local Codex JSON-RPC schema from version 0.164.0.

**Spec:** `docs/superpowers/specs/2026-10-26-codex-team-design.md`

## Global Constraints

- Reuse existing Codex login, profiles and DeepSeek menu credentials; never print or pass secrets as command arguments.
- One owner per subtask; no fixed role per user task. Negotiation/review are read-only; scoped independent writes may run concurrently; no hidden subagent delegation.
- Follow-up instructions reach both members; stopping clears work and prevents late results restarting it.
- Do not modify, stage, reset or commit pre-existing user changes. New tool files live in `tools/codex-team/`.
- No third-party runtime dependencies. Installation outside the writable workspace occurs only after verification through the required filesystem approval.

## Review Focus

- Disagreement must not accidentally open a writable turn; test in Task 2.
- A reviewer requesting escalation must not obtain write permission; test in Task 1.
- Follow-up text arriving while a member is idle must remain unread until delivered; test in Task 2.
- Cancellation during RPC startup and a late result must not restart execution; test in Tasks 1 and 2.
- Reinstallation must not overwrite another `team` command or existing credentials; test in Task 3.

### Task 1: Codex transport and configuration

**Files:** `tools/codex-team/backend.py`, `settings.py`, `tests/test_backend.py`, `tests/fake_app_server.py`, `tests/test_settings.py`.

**Interfaces:** `load_settings(home, environ)` supplies independent backend commands/environments. `CodexBackend.start()`, `turn(prompt, writable=False) -> dict`, `steer(text)`, `interrupt()`, `close()` provide real JSON-RPC lifecycles and streamed public events.

- [ ] Write and run failing tests for profile/credential reuse, a real subprocess speaking JSON-RPC, failed startup, readonly escalation rejection, and interruption.
- [ ] Implement settings and backend with RPC IDs, thread/turn tracking, structured final output, stderr draining, timeouts and cleanup.
- [ ] Run `python3 -m unittest discover -s tools/codex-team/tests -v` and confirm these tests pass.

### Task 2: Ownership and review runtime

**Files:** `tools/codex-team/runtime.py`, `tests/test_runtime.py`.

**Interfaces:** `Team.submit(text)`, `Team.wait()`, `Team.stop()`, `Team.close()`, `Team.status()` use two backends and emit public room events. A task records one executor and the other reviewer.

- [ ] Write and run failing tests for bilateral agreement before writes, one executor, readonly review, repairs by the original owner, follow-up delivery, nonconvergence, and cancellation.
- [ ] Implement bounded proposal/confirmation, a validated dependency graph, optional review, scoped concurrency, replanning with checkpoints, public-message cursors and generation-safe cancellation.
- [ ] Run the whole tool test suite and confirm all tests pass.

### Task 3: Terminal, installation and real checks

**Files:** `tools/codex-team/terminal.py`, `cli.py`, `install.py`, `README.md`, `tests/test_cli.py`, `tests/test_install.py`.

**Interfaces:** `python3 cli.py [-C path] [task]`, `--doctor`, `--check-connection`; `python3 install.py [--home path] [--uninstall]`; installed entry `team`.

- [ ] Write and run failing subprocess/PTY tests for clean interactive input, command handling and exit, one-shot input, offline doctor, and safe repeat installation.
- [ ] Implement a single editable input line with history, visible member/stage labels, status/stop/quit, unified approvals and private public-event logs.
- [ ] Document dynamic allocation, optional review, unique subtask ownership, scoped concurrency and failures/limits.
- [ ] Run the whole tool suite, compile checks, and a fresh code review; resolve material findings with regression tests.
- [ ] Request the necessary filesystem/network execution permission once the installation and connection check are reviewable; install and verify real GPT/DeepSeek requests in a temporary project without user-code changes.

## Execution decisions

- The user has approved implementation of the design. Continue inline without asking again about routine plan/execution choices.
- Source isolation is a new self-contained tool directory; creating a worktree in the ancestor dirty repository would add unrelated git metadata outside the writable workspace. Leave that repository's existing files and index untouched.
- Use outputSchema for coordination because the locally generated stable ThreadStartParams has no dynamicTools field. Treat malformed model responses as errors rather than fabricating agreement.

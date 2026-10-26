# Codex Team Natural Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the task-phase workflow with a natural, unlimited GPT/DeepSeek relay that uses only a hidden floor-control marker.

**Architecture:** Keep the two Codex app-server processes and approval transport. Return plain text turns, pass user input verbatim, exchange labeled transcript deltas, and schedule the next speaker from the hidden marker or a deterministic fallback sequence.

**Tech Stack:** Python 3.11+ standard library; local Codex app-server protocol; `unittest`.

**Spec:** `docs/superpowers/specs/2026-10-26-codex-team-natural-relay-design.md`

## Global Constraints

- Do not modify unrelated project files or the repository index.
- Reuse existing GPT/DeepSeek profiles and credentials.
- Never print command events by default; `/verbose` enables them.
- No fixed discussion-round limit. Stop only on an explicit handoff, user stop, error, or repeated no-progress loop.
- Do not wrap normal user input in task/stage/role prompts.

## Review Focus

- A simple greeting must not cause repository investigation.
- A marker must never appear in the public message.
- A new user message during an active turn must steer, not start a second task.
- Long marker-driven relays must continue beyond the default fallback sequence.
- Command text must remain absent from default terminal output.

### Task 1: Plain-text backend turns

**Files:**
- Modify: `tools/codex-team/backend.py`
- Modify: `tools/codex-team/tests/fake_app_server.py`
- Modify: `tools/codex-team/tests/test_backend.py`

**Interfaces:**
- Produces: `CodexBackend(..., partner=None)` and `await turn(prompt, writable=False) -> str`.
- Produces: one-time `member_instructions(name, partner) -> str` containing only room identity and hidden marker protocol.

- [ ] Write failing tests that expect plain text, exact prompt preservation, and no output schema.
- [ ] Run the backend tests and confirm they fail for the old structured reply.
- [ ] Remove `REPLY_SCHEMA`, return the final text, and replace task-phase instructions with the minimal room charter.
- [ ] Run backend tests and confirm all pass.

### Task 2: Natural relay runtime

**Files:**
- Replace: `tools/codex-team/runtime.py`
- Replace: `tools/codex-team/tests/test_dynamic_runtime.py` with `tests/test_natural_runtime.py`
- Modify: `tools/codex-team/tests/test_cli.py`

**Interfaces:**
- Consumes: `await member.turn(prompt, writable=bool) -> str`.
- Produces: `Team.submit(text)`, `Team.wait()`, `Team.stop()`, `Team.status()`, `parse_handoff(text) -> (clean_text, target|None)`.

- [ ] Write failing tests for verbatim first input, default GPT→DeepSeek→GPT→user fallback, explicit marker stripping, marker-driven extension beyond three turns, early user handoff, and mid-turn steer.
- [ ] Run the runtime tests and confirm they fail against the graph runtime.
- [ ] Implement the relay loop, hidden marker parsing, labeled unread transcript delivery, user interruption, and repeated-loop detection.
- [ ] Run runtime and CLI tests and confirm all pass.

### Task 3: Quiet terminal and documentation

**Files:**
- Modify: `tools/codex-team/terminal.py`
- Modify: `tools/codex-team/cli.py`
- Modify: `tools/codex-team/README.md`
- Modify: `tools/codex-team/install.py` if the file list changes.
- Test: `tools/codex-team/tests/test_cli.py`

- [ ] Write failing tests that command events are hidden by default and `/verbose` restores them.
- [ ] Implement terminal command filtering, `/verbose`, new relay CLI arguments, and connection-check plain text.
- [ ] Update README and help text to describe natural relay and hidden handoff.
- [ ] Run the complete suite, install the tool, and run the installed `team --doctor`.


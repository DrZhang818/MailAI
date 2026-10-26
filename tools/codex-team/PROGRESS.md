# Codex Team progress — plan: docs/superpowers/plans/2026-10-26-codex-team.md

- Design approved; user clarified one executor plus one reviewer after discussion.
- Pre-flight: settings → backend → runtime → terminal interfaces agree with the implementation plan.
- Ruling: work in a new self-contained tools/codex-team directory without touching the ancestor repository's dirty code or index; installation will copy this standalone tool into the user's data directory.
- Ruling: stable local schema supports outputSchema but omits dynamicTools, so structured final coordination is the supported transport.
- Task 1: complete; profile/credential reuse, JSON-RPC subprocess, approvals, interruption and startup errors verified.
- Task 2: complete; agreement, exclusive ownership, readonly review, same-owner repairs and follow-up routing verified.
- Task 3: implementation complete; installation and terminal tests pass; real-model checks pending.
- Fresh read-only review found six material issues. Each now has a regression test and fix: canceled work resurrection, final-review follow-up delivery, blocked controls during steer, unshared public progress, incomplete approval details and misleading doctor auth claims.
- Additional unknown-turn cancellation regression verified RED→GREEN.
- Verification: `python3 -m unittest discover -s tools/codex-team/tests -v` → 27 tests passing, including a full CLI flow with two local protocol fixture subprocesses.
- No existing user code or repository index was changed. No paid model requests have run yet.
- Live check 1: installed `team`; GPT successfully responded. DeepSeek app-server rejected the CLI `--profile` flag (Codex restricts it to runtime commands).
- Ruling: load the existing profile through thread/start config overrides and explicitly select/verify modelProvider; keep credentials off command arguments. Compatibility regression added before fixing.
- Live check 2 passed: both real models connected, GPT created the smoke artifact and DeepSeek verified exact content in a temporary directory. Installed entry and terminal control checks passed.
- User correction: fixed executor/reviewer roles are NOT the objective. Supersede that policy with agreed subtask allocation, optional review, independent concurrency and waiting when no work is needed. Existing transport/UI remain in place; runtime/schema tests are being adapted to the corrected contract.
- User correction: commands must look like normal Codex turns, not phase/task prompts. Natural relay supersedes the task-graph runtime.
- Natural relay implemented: user input is verbatim on the first turn, hidden `[[next:...]]` markers control the floor, default fallback is GPT → DeepSeek → GPT → user, and marker-driven relays have no fixed round cap.
- Terminal now hides command events by default; `/verbose` toggles them. README and help describe the natural relay behavior.

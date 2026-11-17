# satellite-network-sim

A simulator of a Starlink-like satellite network on real public orbital data.

Global rules (workflow, commits, review, self-review) load from
`~/.claude/CLAUDE.md`. Add project-specific sections here as the project grows
(still to come: **Architecture**, **Non-obvious domain rules**, **Conventions**).

## Stack

- **C++ (C++20, CMake, GoogleTest): the engine.** Orbit propagation, frames
  and time, topology, routing, failure simulation. All performance-sensitive
  work lives here.
- **TypeScript: the visual dashboard.**
- **Python: analysis, plots and validation.** Never the performance layer.

## Commands

- **`scripts/check.sh` is the single check command.** It builds the Docker
  image in `docker/Dockerfile` and runs every CI step inside it: C++ format,
  Python lint, ASan + UBSan tests with GCC and with Clang, Valgrind,
  clang-tidy. Exit 0 only if all pass. It needs only Docker on the host.
- `scripts/check.sh <step>` runs one step (`format`, `python-lint`,
  `asan-gcc`, `asan-clang`, `valgrind`, `tidy`).
- CI (`.github/workflows/ci.yml`) runs the same steps through the same
  `scripts/ci.sh`, so a green local check means a green CI. Prefer adding a
  step to `scripts/ci.sh` over putting logic in the workflow file, because
  logic that lives only in the workflow cannot be run locally.
- What each step catches and why: `docs/ci.md`.

## Working model: Fable orchestrates, Opus implements

A strategy being tried on this project. Revisit it if it isn't paying off.

- **Claude Fable 5.2 (the main session) is the orchestrator and architect.**
  It owns design, task breakdown, the briefs given to subagents, review of
  what they return, and verification.
- **Implementation goes to Opus 5.5 subagents** (`model: "opus"` on the Agent
  tool). Give each one a narrow, self-contained brief: the goal, the files it
  may touch, the constraints, and what done looks like (tests, check command).
- **Be selective with subagents, because they burn usage limits.** Spawn one
  only when there is a real, well-scoped implementation unit. Prefer one agent
  per coherent unit of work over wide fan-outs, and parallelize only pieces
  that are truly independent. Trivial edits, small fixes and anything that is
  a handful of tool calls are done directly by Fable rather than delegated.
- **Fable owns this codebase.** Delegating the typing does not delegate the
  responsibility: Fable is accountable for the code being correct, working and
  hygienic. Never accept a subagent's report at face value. Read the diff, run
  the check command, and fix or send back anything that is wrong, sloppy,
  untested or out of scope before it is committed.

## Repo

- Public GitHub repo; GitHub is the source of truth.
- Never commit secrets: everything here is publicly visible.

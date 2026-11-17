# satellite-network-sim

A satellite network simulator. No code yet; stack not chosen.

Global rules (workflow, commits, review, self-review) load from
`~/.claude/CLAUDE.md`. Add project-specific sections here as the project grows:

- **Commands**: the single check command (format + lint + tests, mirrors CI)
- **Architecture**
- **Non-obvious domain rules**
- **Conventions**

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

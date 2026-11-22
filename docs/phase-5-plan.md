# Phase 5 implementation plan

## Context

Phases 1 to 4 are merged (`main` at `c346873`, CI green). The engine is finished. The only
remaining piece the repo names is the visual dashboard in TypeScript. The owner accepted the
nine recommendations put to him on 2026-11-22 and asked for a plan and then the
implementation through to the end of the project.

Traffic load and congestion are out of scope in every spec and are not part of this plan.
Whether they become a phase is the owner's call; nothing here depends on it.

## The owner's nine decisions (to be written into `DECISIONS.md` in his style)

1. **Purpose:** an explainer with guided stories that can also be explored freely.
2. **Data source:** playback of runs the engine saved. No engine in the browser, no server.
3. **Positions:** sampled files from `propagate`, interpolated in the browser, with the
   interpolation error measured and stated. WebAssembly is the fallback if files are too heavy.
4. **Globe:** three.js.
5. **Stories:** (c) one failure under the three reaction modes side by side, (b) one route
   through time under the four strategies, (a) the constellation, shells and laser patterns,
   (d) the network falling apart. Built in the order c, b, a, d.
6. **Time:** a speed slider plus automatic, clearly marked slow motion around a failure.
7. **Data location:** built at deploy time by an export script; not in git.
8. **Hosting:** GitHub Pages (free for this public repo), deployed from `main` only when the
   owner says so. Every data file under 25 MiB, so Cloudflare's free plan stays open.
9. **Proof:** type check, lint, unit tests, a browser test, and a data check that compares
   every exported number with the engine's CSV, as one new step of `scripts/ci.sh`.

## Design

- **`python/export_dashboard.py`** runs the already-built programs (`propagate`, `topology
  --links-out`, `routing`, `failures`) on a small fixed list of scenarios and writes
  `dashboard/public/data/` (git-ignored): `manifest.json`, the satellite list with shell and
  plane, the planned laser links, ground sites, position chunks (binary, a fixed sample step,
  split under 25 MiB), and per story a JSON file of intervals, routes, facts and flood
  learning instants. It reuses the runner helpers of `python/run_phase4.py` and
  `python/make_failures.py` for scenario choice and settings. A `--small` mode exports a
  short window for tests.
- **`python/check_dashboard_data.py`** (the data check) reads the exported files back and
  compares every number with the engine's CSVs, and measures the interpolation error against
  `propagate` at 1 s steps.
- **`dashboard/`**: Vite, TypeScript (strict), three.js, a plot library for the charts. Parts:
  data loaders that validate the manifest and file shapes; a clock (speed, slow motion,
  scrubbing); the globe (Earth, 8,395 satellites as one points object, links as line
  segments, highlighted routes, ground sites); one module per story; chart panels fed by the
  committed summaries in `docs/phase-N-data/`.
- **Engine code is not changed.** If the export needs an output a program does not write
  (for example link states per step), that is added as an option with tests, and the existing
  outputs stay byte-identical.

## Units of work

One Opus subagent at a time (CLAUDE.md working model). I write each brief, read each diff,
run the check and plant bugs before committing. Branch `phase-5-dashboard`, pushed after
every commit.

| # | Unit | Main files | Done when |
|---|---|---|---|
| 0 | Spec and decisions | `docs/phase-5-dashboard.md`, `DECISIONS.md` (Phase 5 section), `docs/phase-5-plan.md` | Written by me in the style of the earlier specs; no open questions in them |
| 1 | Export and data check | `python/export_dashboard.py`, `python/check_dashboard_data.py`, `.gitignore` | Export runs clean; the check passes; a planted wrong number makes it fail; file sizes and interpolation error measured |
| 2 | App skeleton | `dashboard/` (Vite config, loaders, clock, globe with satellites, links, sites) | Loads the small export; unit tests for loaders, clock and interpolation; a browser test renders the globe |
| 3 | Story c: a failure, three modes | `dashboard/src/stories/failure/` | Three lanes side by side in step with one clock; black hole, loop and delivered drawn differently; flood wave and report path shown; measures panel equals `measures.csv` |
| 4 | Story b: a route through time | `dashboard/src/stories/route/` | Four strategies selectable; changes, drops and delay over time; numbers equal the routing CSV |
| 5 | Stories a and d | `dashboard/src/stories/constellation/`, `.../resilience/` | Shells, planes and patterns selectable; the after-recovery charts from the committed summaries |
| 6 | CI and deploy | `docker/Dockerfile` (Node), `scripts/ci.sh` (`dashboard` step), `scripts/check.sh`, `.github/workflows/ci.yml`, `.github/workflows/pages.yml` (manual start only), `docs/ci.md`, `CLAUDE.md` | The step runs type check, lint, unit tests, small export, data check and the browser test; green locally and in CI |
| 7 | Report | `docs/phase-5-results.md` (what was built, how it is checked, sizes, limits, screenshots), README, spec status line | Every number measured |

## Rules that apply

- Check: `scripts/ci.sh all` in the `satsim-ci` image (cloud workaround in CLAUDE.md), now 11
  steps. The dashboard step must not need the network at test time beyond `npm ci`.
- A bug becomes a failing test first. Planted bugs prove new tests can fail.
- Nothing shown is computed in the browser except interpolation and drawing: every number on
  screen comes from an engine file or a committed summary.
- No secrets, no tokens, no third-party tracking in the site. Map imagery must be free to
  redistribute (a plain Natural Earth texture or drawn coastlines), with its licence noted.
- Screenshots for the report are taken by the browser test, not by hand.

## What needs the owner

- **The pull request and merge:** he asked for the implementation through to the end. I open
  the pull request when the full check is green and merge by rebase when CI is green, as for
  Phase 4.
- **Going public:** turning GitHub Pages on and the first deploy happen only when he says so
  (decision 8). If the repository setting cannot be changed from here, it is one switch for
  him in the repository settings, and I say exactly which.
- Anything that contradicts a decision, or any decision not in the nine, is put to him in chat.

## Risks

- **Position files too heavy.** Measured in unit 1. If a two-hour window cannot be brought
  under about 50 MB with a coarser step and better interpolation, the stories use shorter
  windows; WebAssembly stays the fallback and would be put to the owner first.
- **Browser test flakiness in CI** (software rendering of WebGL). The test asserts on the
  app's state and on a coarse screenshot comparison, not on exact pixels.
- **Usage limits.** One subagent at a time; small fixes done directly.

## Verification, end to end

- `scripts/ci.sh all`: 11 of 11 steps pass, including `dashboard`.
- `python3 python/export_dashboard.py` then `python3 python/check_dashboard_data.py`: zero
  differences from the engine's CSVs; the interpolation error is within what the spec states.
- `npm run build` and `npm run preview` in `dashboard/`, opened in the pre-installed browser:
  each of the four stories loads, plays, scrubs and shows the numbers of its summary file.
- CI green on the pull request; after the owner's go-ahead, the Pages workflow deploys and
  the public page loads.

# Phase 5: The dashboard

**Status:** specified 2026-11-22, implemented and published 2026-11-23 at
<https://teodortopan.github.io/satellite-network-sim/>. Results:
[phase-5-results.md](phase-5-results.md). The work plan is in [phase-5-plan.md](phase-5-plan.md).

Sources opened on 2026-11-22:
[GitHub Pages limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits),
[Cloudflare Pages limits](https://developers.cloudflare.com/pages/platform/limits/),
[three.js documentation](https://threejs.org/docs/),
[Natural Earth terms of use](https://www.naturalearthdata.com/about/terms-of-use/),
[satellitemap.space](https://satellitemap.space/) and
[Hypatia's visualisations](https://github.com/snkas/hypatia) as examples of what exists.

## 1. Problem

Phases 1 to 4 produce numbers, tables and plots. They answer the questions of each phase, but
nobody can watch what happens: where the satellites are, which laser links carry a route, how
a route moves from satellite to satellite, and what the three reaction modes do in the seconds
after a failure.

Phase 5 is a web page that shows this. It is an explainer: four stories, each with a short
text, a globe and a few charts, and each can also be explored freely (turn the globe, move
through time, pick a pair or a mode).

The public tools that exist show satellite positions, and Hypatia draws single frames. None
shows laser links, routes, delay and failures together on real data.

The page must never show a number the engine did not compute. That is the main rule of this
phase, and section 6 says how it is checked.

## 2. Background: the terms this spec uses

Terms from earlier phases (shell, plane, laser pattern, strategy, reaction mode, lane, black
hole, loop, fact, flood, report, re-pointing) keep the meaning given in the specs of Phases 2
to 4.

- **Playback.** The page replays files that the engine wrote before. Nothing is simulated in
  the browser.
- **Export.** The step that runs the engine's programs on a fixed list of scenarios and turns
  their CSV files into the files the page loads.
- **Exported data.** The files under `dashboard/public/data/`. They are built, not committed.
- **Manifest.** `manifest.json`, the list of every exported file with its size, its sha256 and
  the settings it was made with. The page loads the manifest first and everything else through
  it.
- **Sample step.** The time between two saved positions of a satellite.
- **Interpolation.** Computing a position between two saved samples. It is the only arithmetic
  on simulation data the browser does.
- **Story.** One part of the page: a text, a globe view, charts, and its own data file.
- **Clock.** The one simulated time of a story. Everything on screen is drawn for the clock's
  time.
- **Slow motion.** A span of simulated time that the clock plays slower than the chosen speed,
  because the events in it are milliseconds apart.
- **Data check.** A Python program that reads the exported data back and compares it with the
  engine's CSV files.
- **Browser test.** A test that opens the built page in a real browser without a screen and
  checks what it shows.

## 3. What is already known

- The snapshot has 8,395 working satellites. One position at 1 s steps for two hours is about
  725 MB as numbers; nobody can download that. Sampled every 60 s as 32-bit numbers it is
  about 12 MB.
- A satellite moves about 7.6 km per second on a near-circle. Between two samples the
  straight line misses the true position by hundreds of metres to kilometres; following the
  arc of a circle through both samples misses by far less. The exact error is measured (6.2).
- three.js draws 16,000 to 25,000 points and as many line segments at full frame rate on an
  ordinary laptop. One points object for all satellites is the standard way.
- `results/phase-4` is 954 MB, so the page cannot load the runs as they are. One failure run
  of one pair is small: a few lanes, a few hundred intervals, and one learning instant per
  satellite for the flood.
- GitHub Pages is free for a public repository: a site of up to 1 GB, a soft limit of 100 GB
  of traffic per month, a deploy of up to 10 minutes. It cannot set the headers that threaded
  WebAssembly needs. Cloudflare Pages' free plan allows 25 MiB per file and 20,000 files.
- Natural Earth map data is in the public domain. The npm package `world-atlas` carries it
  ready-made.
- The engine's programs already write almost everything the stories need: `propagate`
  (positions), `constellation` (shell and plane of each satellite), `topology --links-out`
  (laser links), `routing --out --drops-out` (routes and drops over time), `failures`
  (`--events-out`, `--facts-out`, `--floods-out`, `--measures-out`, `--network-out` and the
  routes).

## 4. Design

### 4.1 Parts

| Part | Where | Job |
|---|---|---|
| Export | `python/export_dashboard.py` | Runs the built programs on the scenario list and writes the exported data |
| Data check | `python/check_dashboard_data.py` | Reads the exported data back and compares it with the engine's CSV files; measures the interpolation error |
| Page | `dashboard/` | Vite, strict TypeScript, three.js for the globe, uPlot for the charts |
| Deploy | `.github/workflows/pages.yml` | Builds the engine, runs the export, builds the page, publishes it. Started by hand only |

The engine's code is not changed. If the export needs an output that no program writes, the
output is added as an option with its own tests, and every existing output stays the same
byte for byte.

### 4.2 The exported data

Everything is under `dashboard/public/data/`, which is ignored by git.

- `manifest.json`: a format version, the snapshot date, the export's settings (window start,
  window length, sample step, `small` or `full`), and for every other file its path, size in
  bytes and sha256.
- `satellites.json`: for each satellite its catalogue number, name, shell and plane, in the
  order used by every other file. A satellite the engine gives no shell is left out, as in
  Phases 2 to 4.
- `sites.json`: the cities of the study pairs, the gateways and the PoPs, each with name,
  kind, latitude and longitude.
- `links-<pattern>.json`: the planned laser links of one pattern as pairs of satellite
  indices, for `plus-grid`, `three` and `six`.
- `positions-<n>.bin`: positions in the Earth-fixed frame (ECEF) in kilometres, as 32-bit
  little-endian floats, sample after sample, satellite after satellite, three numbers each.
  A file is cut before it reaches 25 MiB. The manifest gives each file's first sample and
  number of samples. A satellite whose propagation failed at a sample has three NaN there and
  is not drawn.
- `story-failure.json`, `story-route.json`, `story-constellation.json`,
  `story-resilience.json`: one file per story (4.5 to 4.8).

Times in the story files are seconds from the window start, as the engine writes them. A
satellite is named by its index in `satellites.json`, a ground site by its index in
`sites.json`.

A 32-bit float holds a position of 7,000 km to about half a metre. That is far below what a
screen can show and is counted in the measured error.

**Window.** The full export covers two hours from the plan time of Phases 3 and 4, at a
sample step of 60 s. The small export (`--small`) covers seven minutes starting where the
failure story's run starts, so that every story has positions, and runs each story's
shortest scenario; it is what the tests and CI use. Both are settings of
the export and are written into the manifest.

### 4.3 Interpolation

Between two samples at times `t0` and `t1` the page computes the position at `t` like this:

1. Turn the later sample back around the Earth's axis by the angle the Earth turned in
   `t1 - t0` (the rotation rate is the constant 7.292115e-5 rad/s). Both samples are now in a
   frame that does not turn during the step, where the orbit is close to a circle around the
   Earth's centre.
2. Move along the great-circle arc between the two directions by the share
   `(t - t0) / (t1 - t0)`, and take the distance from the centre as the straight mix of the
   two distances.
3. Turn the result forward around the Earth's axis by the angle the Earth turned in `t - t0`.

At a sample time the result is the sample itself. If either sample is NaN the satellite is
not drawn for that step. The clock never runs outside the window, so nothing is extrapolated.

### 4.4 The page

- **Loading.** The manifest first. Every other file is checked against the manifest's size
  before use, and every JSON file's shape is validated (required fields, types, index ranges)
  before anything is drawn. A file that fails is reported on the page in plain words; nothing
  is drawn from it.
- **Clock.** Play, pause, a speed slider (1x to 600x simulated seconds per real second), and
  a time bar to move anywhere in the story's window. A story can mark spans as slow motion
  with a fixed speed; while the clock is in such a span the speed shown on screen is the slow
  one, with the label "slow motion". Moving the time bar by hand always works.
- **Globe.** A dark sphere with Natural Earth land outlines. All satellites are one points
  object, coloured by shell. Laser links are line segments between interpolated positions.
  A route is drawn as a thicker line through its sites and satellites. Ground sites are
  markers. The mouse or a finger turns and zooms the globe.
- **Layout.** A story menu, the story's text, the globe, the clock, and the charts. The page
  works at phone width; on a narrow screen the lanes of the failure story are stacked.
- **Numbers.** Every number on the page is read from a story file or from a committed summary
  in `docs/phase-N-data/` that the export copies into the story file. The page formats
  numbers (rounding, units) and does nothing else to them.
- **Nothing else.** No trackers, no cookies, no fonts or scripts from other sites, no tokens.

### 4.5 Story: one failure, three reactions

The first story built. It replays `failures` runs with Phase 4's default settings on the
realistic model, pattern `three`, for one study pair, with the three reaction modes side by
side on one clock.

- **Scenarios** (a menu): a satellite on the live route fails, a whole region fails, and a
  gateway fails; the same definitions as Phase 4's `route-satellite`, `region` and `gateway`
  at instant T1.
- **Three lanes**, one per mode (planner, local-repair, flooding), each a globe view of the
  same region with the same camera. In each lane the direction's route is drawn in the state
  the engine gives it for the clock's time: delivered, black hole (drawn up to the point
  where traffic is lost) or loop. The baseline route (no failure) is drawn faintly beneath.
- **News.** For flooding, each satellite changes colour at the instant the flood reached it.
  For planner and local-repair, the satellites that detected the failure are marked when
  they detect it, and the instant the planner knew and the instant the new plan takes effect
  are shown, with the number of reports and the hops of the first one. The satellites a
  report passes on its way are not shown, because no program writes them. A list beside the
  lanes names each fact and when each actor learned it.
- **Slow motion.** Two spans, after a pause just before the failure. The first second after
  the failure plays at 0.02x, because the flood and the first repairs are milliseconds
  apart. From there until the last lane has settled the clock plays at no more than 2x. The
  spans and their speeds are in the story file.
- **Measures panel.** For each mode: time lost, time to restore, time to settle, extra delay,
  reports and messages, exactly the values in the run's `measures.csv`.
- **Context chart.** The distribution of time lost per mode over all of Phase 4's main runs
  of that scenario, from `docs/phase-4-data/measures.csv`, with this run marked.

### 4.6 Story: a route through time

Replays a `routing` run with Phase 3's main settings on the best-case model over the window,
for the six study pairs and the four strategies (the threshold strategy at 2 ms). The
realistic model is not used here: five of the six pairs have no laser hop in it, so all
strategies give the same route.

- A menu for the pair and the direction, and a switch for each strategy.
- The route of each chosen strategy on the globe at the clock's time, one colour per
  strategy. A drop is drawn as a broken route for as long as it lasts, with its cause named.
- A chart of delay over time per strategy, with the clock's time marked and route changes as
  ticks. A table per strategy: median delay, number of route changes, number of drops and
  time dropped, as the engine's summary gives them.

The story file holds, per direction and strategy, intervals `(from, to, route, delay)` where
the route stays the same, the delay at each engine step, and the drops with their causes.

### 4.7 Story: the constellation

- All satellites over the window, coloured by shell; a menu to show one shell or one plane.
- A switch for the laser pattern (`plus-grid`, `three`, `six`) that draws that pattern's
  planned links.
- Ground sites: cities, gateways and PoPs, each kind switchable.
- A table per shell: number of satellites, number of planes, height and inclination, from
  the `constellation` program's output.

### 4.8 Story: the network falling apart

- Charts from Phase 4's committed summaries (`recovery_summary.csv`, `percolation.csv`): the
  share of pairs still connected against the failed share, for random satellites, random
  lasers, the attack, each pattern, with and without re-pointing; and the size of the largest
  connected part against the failed share.
- A globe view for a menu of failed shares (1%, 10%, 30% random satellites, and the attack at
  30%), one seed each: the failed satellites are greyed out, the planned laser links that did
  not fail are drawn (labelled as such: a planned link can also be out of sight at an
  instant, and no program writes that), and for the six study pairs the route after recovery
  or "no route". These come from
  one-step `failures` runs made by the export with Phase 4's settings.

### 4.9 Deploying

- `npm run build` in `dashboard/` writes a static site into `dashboard/dist/`. The site uses
  relative paths only, so it works under `/satellite-network-sim/` on GitHub Pages and at the
  root of any other host.
- `.github/workflows/pages.yml` starts only by hand (`workflow_dispatch`), only from `main`.
  It builds the engine in the CI image, runs the full export and the data check, builds the
  page and publishes it to GitHub Pages. It uses no secret beyond the token GitHub gives the
  run.
- Every exported file is under 25 MiB and the site has far fewer than 20,000 files, so the
  same `dist/` can be put on Cloudflare Pages' free plan without changes.

## 5. Runs

The export makes these engine runs, all on the snapshot of Phases 1 to 4:

| For | Program | Full export | Small export |
|---|---|---|---|
| Positions | `propagate` | two hours, 60 s step | seven minutes, 60 s step |
| Positions for the error measurement | `propagate` | the first ten minutes at 1 s | the seven minutes at 1 s |
| Satellites | `constellation` | once | once |
| Laser links | `topology --links-out` | three patterns | three patterns |
| Route through time | `routing` | two hours, six pairs, four strategies | seven minutes, one pair |
| One failure | `failures` | three scenarios, one pair, realistic | one scenario |
| Falling apart | `failures`, one step | four failed shares | one failed share |

The export keeps the engine's CSV files in `results/dashboard/` so that the data check can
read them. It fails if any program exits with an error or reports a failed check.

## 6. How it is shown to be right

### 6.1 The data check

`python/check_dashboard_data.py` reads the exported data and the CSV files in
`results/dashboard/` with code of its own (it shares no parsing or conversion code with the
export) and compares:

- every manifest entry with the file on disk (size and sha256), and that no file is 25 MiB or
  larger;
- every satellite's shell and plane with the `constellation` output;
- every position sample with the `propagate` CSV, to the precision of a 32-bit float;
- every laser link of every pattern with `--links-out`;
- every interval, state, route, delay, drop, fact, flood instant and measure in the story
  files with the engine's CSV rows, exactly for whole numbers and names and to the last
  written digit for decimals;
- every number copied from `docs/phase-N-data/` with the committed file.

One difference fails the check and is printed with the file, the row and both values.

### 6.2 The interpolation error

The data check implements 4.3 in Python and compares it with `propagate` at 1 s steps for
every satellite. It prints the median and the largest error and fails if the largest is over
1 km. One kilometre is 1/6,400 of the Earth's radius, less than a tenth of a pixel on a globe
that fills a large screen. The measured values go into the report.

The page's own interpolation is tested against numbers the Python version produced, so the
two cannot drift apart.

### 6.3 Tests of the page

- **Type check and lint.** TypeScript in strict mode with no errors; ESLint with no warnings.
- **Unit tests** (Vitest): the loaders (a good file loads; a wrong size, a missing field, an
  index out of range are each refused), interpolation (equals the samples at sample times;
  equals the reference numbers; NaN is not drawn), the clock (speed, pause, limits of the
  window, slow motion spans), and each story's lookup of "what is the state at time t"
  against hand-made intervals.
- **Browser test** (Playwright, Chromium without a screen): the built page is served with
  the small export. For each story the test checks that it loads without an error on the
  console, that the globe drew something (the canvas is not one flat colour), that moving the
  clock changes the state the page reports, and that numbers on screen equal the values in
  the engine's CSV files, which the test reads itself. The test also takes the screenshots
  used in the report.
- **Planted bugs.** As in every phase, bugs are planted one at a time (a wrong index, a
  swapped axis, a skipped interval, a number changed in the export) and a named test or the
  data check has to fail on each.

### 6.4 One step of the check

`scripts/ci.sh dashboard` builds the engine, runs the small export and the data check,
installs the page's packages from the lock file, and runs the type check, the lint, the unit
tests, the build and the browser test. It is part of `scripts/check.sh` and of CI. The CI
image gets Node and Chromium for it.

## 7. Out of scope

- Simulating anything in the browser: no SGP4, no routing, no failures there.
- A server, accounts, or data that changes after the deploy. The page shows the snapshot of
  Phases 1 to 4.
- Free choice of failures by the visitor. The scenarios are the exported ones.
- Traffic load, congestion and packet loss, as in every earlier phase.
- Satellite imagery, terrain and day and night shading.
- Exact pictures: laser beams, dish beams and satellite shapes are drawn as lines and dots.

# The dashboard's exported data

The files `python/export_dashboard.py` writes into `dashboard/public/data/` (spec
[docs/phase-5-dashboard.md](../docs/phase-5-dashboard.md) 4.2). They are built, not committed.
`python/check_dashboard_data.py` compares every value in them with the engine's CSV files.

## Conventions

- **JSON** is UTF-8, one value per file, with no `NaN` or `Infinity`. A number may be written
  with or without a fraction (`67` and `67.0` are the same number).
- **Absent values are `null`**: a measure the engine writes as `none`, an instant it writes as
  `never`, an empty CSV field.
- **Decimals are the engine's own.** Each is the number the program wrote in its CSV, read
  back exactly (it round-trips as a 64-bit float). Nothing is rounded. The page rounds only
  for display.
- **A satellite** is named by its index in `satellites.json` (0-based), called `sat` below.
- **A ground site** is named by its index in `sites.json` (0-based), called `site` below. A
  field that holds a site of one kind says which (`site:city`, `site:gateway`, `site:pop`).
- **Times.** There are two clocks.
  - **Window time:** seconds from the window start, `manifest.settings.window_start_utc`.
    Position samples are on it. It runs from 0 to `manifest.settings.window_s`.
  - **Story time:** seconds from the start of that story's engine run, as the engine writes
    them (`story-route.json` and every scenario of `story-failure.json`). Each of these gives
    `run_start_s`, its run's start in window time, so **window time = `run_start_s` + story
    time**. `story-resilience.json` steps are single instants and give `t_s` directly in
    window time.

  | Export | Window start (UTC) | `window_s` | Route story `run_start_s` | Failure story `run_start_s` | Resilience step `t_s` |
  |---|---|---|---|---|---|
  | full | 2026-11-18T21:15:00 (the plan time) | 7200 | 0 | 930 | 300 (T0, 21:20:00) |
  | small | 2026-11-18T21:30:30 (the failure run's start) | 420 | 0 | 0 | 67 (T1, 21:31:37) |

  Every story's whole range (`run_start_s` to `run_start_s + duration_s`, or `t_s`) lies
  inside `[0, window_s]`; the data check verifies it. The shells, planes and laser plans are
  those of the plan time 2026-11-18T21:15:00 in both exports
  (`manifest.settings.plan_time_utc`). UTC texts are `YYYY-MM-DDTHH:MM:SS[.f]`, UTC.
- **Units** are in the field names: `_s` seconds, `_ms` milliseconds, `_km` kilometres,
  `_deg` degrees. A `share` is a fraction from 0 to 1.
- **An element** (something that fails or detects) is an array whose first item is its kind:

  | Array | Meaning |
  |---|---|
  | `["satellite", sat]` | a satellite |
  | `["link", sat, sat]` | a laser link (lower catalogue number first, as the engine writes it) |
  | `["gateway", site:gateway]` | a gateway |
  | `["city", site:city]` | a city's dish (detectors and flood nodes only) |

- **A route** is an array of `sat`, in order from the sending end. The cities at the two ends
  are the direction's `from` and `to`. In the best case the last satellite links down to the
  receiving city. In the realistic model the first satellite is the sender's serving
  satellite, and the path continues from the last satellite down to its `gateway`, then over
  fibre to the sender's home PoP, then over fibre to the receiving city (Phase 2 spec 4.6).

Values the export works out itself, rather than copying, are marked **(computed)** below;
the data check recomputes each one on its own.

## `manifest.json`

Loaded first; every other file is reached through it.

| Field | Type | Meaning |
|---|---|---|
| `format_version` | integer | `1` |
| `snapshot` | string | The snapshot date, `2026-11-18` (`data/starlink/<date>/`) |
| `ground` | string | The ground-data date, `2026-11-19` (`data/ground/<date>/`) |
| `settings.mode` | `"small"` or `"full"` | Which export |
| `settings.plan_time_utc` | string | The reference time of the shells, planes and laser plans, the plan time of Phases 3 and 4: `2026-11-18T21:15:00` |
| `settings.window_start_utc` | string | The window start: the plan time (full) or the failure story's run start `2026-11-18T21:30:30` (small) |
| `settings.window_s` | integer | Window length: 7200 (full) or 420 (small) |
| `settings.sample_step_s` | integer | Time between two position samples: 60 |
| `settings.samples` | integer | Number of samples, `window_s / sample_step_s + 1` (both ends included) |
| `settings.error_window_s` | integer | The span over which the interpolation error was measured, from the window start: 600 (full) or 420 (small) |
| `settings.error_step_s` | integer | The step of that measurement: 1 |
| `satellites` | integer | Number of satellites, `N` below (8,395) |
| `positions` | array | One entry per position file, in sample order: `path` (string), `first_sample` (integer, index of its first sample), `samples` (integer). Together they cover samples `0 .. samples - 1` exactly once |
| `files` | array | Every other file of the export (position files and the files under `engine/` included, the manifest itself not): `path` (string, relative to the manifest, `/` between directories), `bytes` (integer), `sha256` (string, lower-case hex) |
| `engine` | object | The routing program in WebAssembly and its inputs; see [The engine](#the-engine-engine) |

A loader must refuse a file whose size is not `bytes`.

## `satellites.json`

The satellites the `constellation` program puts in a plane at the plan time, in catalogue
order. A satellite with no shell or plane is left out. One object of parallel arrays, each of
length `N`; index `i` of every array is satellite `i`.

| Field | Type | Meaning |
|---|---|---|
| `shells` | array of string | The shell names (`53deg`, `43deg`, `97deg`, `70deg`) in the order of the `constellation` summary |
| `norad_cat_id` | array of integer | Catalogue number |
| `name` | array of string | Object name (`STARLINK-1008`) |
| `shell` | array of integer | Index into `shells` |
| `plane` | array of integer | Plane index within the shell |
| `index_in_plane` | array of integer | Place in the plane |

## `sites.json`

`{"sites": [ ... ]}`, an array of objects in this order: the cities of the study pairs (in
`data/ground/cities.json` order), every gateway of `gateways.json` then every gateway of
`assumed_gateways.json`, then every PoP of `pop_codes.json`. `(kind, name)` is unique.

| Field | Type | Kinds | Meaning |
|---|---|---|---|
| `name` | string | all | City name; gateway name as the engine writes it; PoP code (`lhr`) |
| `kind` | `"city"`, `"gateway"` or `"pop"` | all | |
| `lat_deg`, `lon_deg` | number | all | Latitude and longitude (WGS-84), from the ground file |
| `country` | string | city, pop | ISO country code |
| `status` | `"operational"` or `"planned"` | gateway | Only operational gateways carry traffic in the engine |
| `source` | `"real"` or `"assumed"` | gateway | `assumed`: placed at a PoP (`make_assumed_gateways.py`) |
| `city` | string | pop | The PoP's city |

## `places.json`

The places a visitor of "Try it" can find by name: Natural Earth's populated places (1:10m,
simple; public domain), made by `python/fetch_places.py` into `data/ground/places/places.json`
and copied unchanged (the data check compares the two byte for byte).

`{"fields": ["name", "region", "country", "lat_deg", "lon_deg", "population"], "rows": [ ... ]}`,
one row per place, largest population first (7,342 rows). The page refuses the file unless
`fields` is exactly that list and every row has six items: `name` (a non-empty string),
`region` (string, may be empty), `country` (string), `lat_deg` (number, -90 to 90), `lon_deg`
(number, -180 to 180) and `population` (whole number, 0 or more).

## `links-<pattern>.json`

One file each for `plus-grid`, `three` and `six`: the planned laser links of that pattern at
the plan time (`topology --links-out`), in the file's order.

| Field | Type | Meaning |
|---|---|---|
| `pattern` | string | The pattern |
| `links` | array of `[sat, sat]` | One laser link each |

These are planned links. Whether a link is in sight at a given time is not in any program's
output; a link of a satellite that is down (story 4.8) is down.

## `positions-<n>.bin`

Earth-fixed (ECEF, ITRF) positions in kilometres, from `propagate`. 32-bit IEEE floats,
little-endian, no header. Sample after sample, satellite after satellite (`satellites.json`
order), `x, y, z` each:

```
float index of (sample s, satellite i, axis a) in the file =
    ((s - first_sample) * N + i) * 3 + a          a: 0 = x, 1 = y, 2 = z
```

Sample `s` is at window time `s * sample_step_s`. A file holds whole samples and is under
25 MiB (`bytes = samples * N * 12`). A satellite with no position at a sample (its propagation
failed) has three NaN there and is not drawn for the steps that touch that sample.

**Interpolation** (spec 4.3), between samples `s0` and `s1 = s0 + 1` at `t0` and `t1`, for
`t0 <= t <= t1`, with `w = 7.292115e-5` rad/s and `Rz(a)` the rotation by `a` about `z`
(`x' = x cos a - y sin a`, `y' = x sin a + y cos a`):

1. `q1 = Rz(w (t1 - t0)) p1`: the later sample in the Earth-fixed axes of `t0`.
2. `f = (t - t0) / (t1 - t0)`; direction: the great-circle (spherical linear) mix of
   `p0 / |p0|` and `q1 / |q1|` by `f`; distance: `(1 - f) |p0| + f |q1|`; `q` is their product.
3. The position is `Rz(-w (t - t0)) q`.

At a sample time this is the sample itself. Measured against `propagate` at 1 s, the error is
printed by the data check (median and largest; it fails above 1 km).

## `story-constellation.json` (spec 4.7)

| Field | Type | Meaning |
|---|---|---|
| `sources` | object | The engine files read (in `results/dashboard/<mode>/`) |
| `plan_time_utc` | string | The reference time of shells, planes and laser plans |
| `shells` | array | One per shell, in `satellites.json` `shells` order, as the `constellation` summary prints it: `shell` (string), `inclination_deg` (the band's centre), `inclination_tolerance_deg` (half-width), `height_min_km`, `height_max_km` (the height band), `plane_gap_deg`, `min_plane_size` (integer; smaller groups are not planes), `satellites_in_bands` (integer), `satellites_in_planes` (integer), `planes` (integer), `plane_size_min` (integer), `plane_size_median` (number), `plane_size_max` (integer) |
| `patterns` | array | One per pattern, `plus-grid`, `three`, `six`: `pattern`, `links` (integer, from the `topology` summary), `by_kind` (object: `in_plane`, `cross_plane`, `second_cross_plane` to integer), `file` (`links-<pattern>.json`) |

## `story-route.json` (spec 4.6)

| Field | Type | Meaning |
|---|---|---|
| `sources` | object | `routes`, `drops`, `summary`: the engine files |
| `settings` | object | `model` (`best-case`), `pattern` (`three`), `replan_s` (15), `planner` (`start`), `backup` (`on`), `step_s` (1), `start_utc`, `duration_s` (7200 full, 420 small), `strategies` (array of string: `lowest-delay`, `keep-until-broken`, `longest-lived`, `threshold:2ms`) |
| `run_start_s` | number | The run's start in window time (0 in both exports) |
| `directions` | array | One per direction (each pair both ways): `from`, `to` (`site:city`), `strategies` (object keyed by strategy name) |

Per strategy:

| Field | Type | Meaning |
|---|---|---|
| `intervals` | array of `[first_s, last_s, route, gateway]` | **(computed)** Each run of consecutive steps that are `ok` with the same route and gateway: its first and last step (both included, integers), the route, and the gateway (`site:gateway`, or `null` in the best case). The state at clock time `t` is the state of step `floor(t)`; a step in no interval has no route |
| `delay_ms` | array of number or null | The delay at every step, `steps` long (index = step = seconds from the start); `null` where the step has no route |
| `drops` | array of `[start_s, length_s, cause, element]` | Every drop event of the drops CSV: start **(computed from its UTC)**, length, cause (`ground_link_lost`, `serving_left_view`, `laser_link_down`, `no_path`, ...), and what caused it: `["satellite", sat]`, `["link", sat, sat, reason]`, `[site_kind, site]` (`city` or `gateway`), or `null` |
| `summary` | object | From the `routing` summary: `ok_steps`, `steps` (integers), `delay_median_ms` (number as printed, 3 decimals; `null` with no delay), `bad_changes`, `increases`, `breaks`, `recoveries`, `drop_events` (integers), `no_route_s` (object: cause to seconds), and **(computed)** `changes` (= `bad_changes + increases`, every route change) and `no_route_total_s` (the sum of `no_route_s`) |

## `story-failure.json` (spec 4.5)

| Field | Type | Meaning |
|---|---|---|
| `settings` | object | Phase 4's defaults: `model` (`realistic`), `pattern`, `strategy` (`lowest-delay`), `step_s`, `replan_s`, `planner` (`whole-interval`), `detection_s`, `hop_processing_s`, `compute_s`, `planner_processing_s`, `acquisition_s`, `repoint`, `replan_on_report` |
| `from`, `to` | `site:city` | The study direction (Reykjavik to Vladivostok) |
| `instant`, `instant_utc` | integer, string | Phase 4's failure instant T1 |
| `scenarios` | array | One per scenario (full: `route-satellite`, `region`, `gateway`; small: `route-satellite`) |

Per scenario:

| Field | Type | Meaning |
|---|---|---|
| `scenario` | string | Phase 4's scenario name |
| `sources` | object | The engine files read |
| `start_utc`, `duration_s` | string, integer | The run's start and length (367 s) |
| `run_start_s` | integer | The run's start in window time (930 full, 0 small) |
| `failure_s` | number | The failure instant t_f, seconds from the run's start (67) |
| `failed` | object | What the failure file fails: `satellites` (array of `sat`), `links` (array of `[sat, sat]`), `gateways` (array of `site:gateway`) |
| `home_pop` | `site:pop` | The sender's home PoP |
| `lanes` | object | Keyed `baseline` (no failure), `reference` (a planner that knows everything), `planner`, `local-repair`, `flooding`: each an array of intervals from the events CSV, in time order, tiling the run |
| `plans` | object | Keyed like `lanes`: every re-plan instant of that lane (in `flooding`, when every node works out its tables again) as `[t_s, what]`, `what` being `replaced`, `kept` or `none` (the routes CSV's `plan` column). The new plan after a failure takes effect at the first `replaced` at or after the instant the planner knows (`facts[].planner_known_s`) |
| `measures` | object | Keyed `planner`, `local-repair`, `flooding`, the measures CSV: `failure_s`, `black_hole_s`, `loop_s`, `lost_s`, `restore_s`, `settle_s` (seconds), `extra_min_ms`, `extra_median_ms`, `extra_max_ms`, `extra_settled_ms`, `reports`, `messages` (integers); `null` where the engine writes `none` |
| `facts` | array | The facts CSV, see below |
| `flood` | object | **(computed)** `satellite_first_learn_s`: array of length `N`, per satellite the earliest instant it learned any fact of this run's flood (the floods CSV), `null` if it never did; `site_first_learn_s`: the same per site, length of `sites.json` (only gateways and, in the best case, cities can be flood nodes) |
| `slow_motion` | array of `{from_s, to_s, speed}` | Spans `[from_s, to_s)` in story time with their speed in simulated seconds per real second. First span: from t_f to t_f + 1 at `0.02`. Second span (only when the last lane settles later than that): from t_f + 1 to **(computed)** t_f plus the largest `settle_s` of the three modes (or the run's end if one of them never settles), speed `2`, which is a cap: the clock plays there at the smaller of the chosen speed and 2x. The speeds are settings of the export |
| `pause_before_failure_real_s` | number | Real seconds the clock pauses just before t_f (a setting, 2) |
| `context` | object | The context chart: `source` (`docs/phase-4-data/measures.csv`), `this_run` (the Phase 4 run name of this scenario, T1, realistic, this pair), `rows`: every row of that file with group `main`, this scenario and variant `default`, as `{run, model, from, to, instant, reaction, lost_s}` (`from`, `to` city names) |

A lane interval is `[start_s, end_s, state, drop_cause, failed_element, one_way_ms, route,
serving, gateway]`:

| Item | Type | Meaning |
|---|---|---|
| `start_s`, `end_s` | number | The interval `[start_s, end_s)` |
| `state` | string | `delivered`, `black_hole`, `loop` or `no_route` |
| `drop_cause` | string or null | `failed_satellite`, `failed_link`, `failed_gateway`, or a Phase 3 cause |
| `failed_element` | element or null | The failed element the traffic meets first |
| `one_way_ms` | number or null | The delay at the interval's start, when delivered |
| `route` | route | The route the traffic was given: for `black_hole` and `loop` in `flooding`, the walk up to the node before the dead element (or the node met twice); in `planner` and `local-repair` the held route. Traffic is lost at `failed_element` |
| `serving` | `sat` or null | The dish's serving satellite (realistic) |
| `gateway` | `site:gateway` or null | The gateway |

A fact is an object:

| Field | Type | Meaning |
|---|---|---|
| `fact` | integer | Its number (0 first) |
| `subject` | element | What it is about |
| `state` | `"down"` or `"up"` | |
| `t_s` | number | When it happened |
| `old` | boolean | Old news, known to everyone from the start |
| `detectors` | array of `[kind, index, t_s]` | Each detector (`satellite` with a `sat`, `city` or `gateway` with a `site`) and when it detected |
| `planner_known_s` | number or null | When the planner knew |
| `reports`, `report_hops` | integer or null | Reports that reach a gateway; hops of the first one |
| `flood_nodes` | integer or null | Nodes the flood reached |
| `flood_last_s` | number or null | When the last one learned |
| `messages` | integer or null | Sendings of this fact |

The satellites a report passes on its way to the planner are not in any program's output and
are not exported; the page shows the detectors and their instants, `planner_known_s`,
`reports`, `report_hops` and, from `plans`, the instant the new plan takes effect.

## `story-resilience.json` (spec 4.8)

| Field | Type | Meaning |
|---|---|---|
| `charts.recovery_summary` | table | `docs/phase-4-data/recovery_summary.csv` |
| `charts.percolation` | table | `docs/phase-4-data/percolation.csv` |
| `steps` | array | One per one-step run (full: random satellites 1%, 10%, 30% with seed 0, and the attack at 30%, at T0; small: random satellites 1%, seed 0, at T1) |

A table is `{source, columns, rows}`: the committed file's path, its header (array of string),
and its rows (arrays in column order), each cell an integer or a number where the text is
one, a string otherwise, `null` where empty. Column meanings are in `python/run_phase4.py`
(`recovery_summary.csv`) and `python/compare_failures.py` (`percolation.csv`).

Per step:

| Field | Type | Meaning |
|---|---|---|
| `name` | string | `<scenario>.share-<share>.seed-<seed>` or `attack.share-<share>.T<instant>` |
| `sources` | object | The engine files read |
| `scenario` | string | `random-satellites` or `attack` |
| `share` | number | The failed share of the satellites in planes |
| `seed` | integer or null | The random draw's seed (`null` for the attack) |
| `instant`, `utc` | integer, string | Phase 4's instant of the step: T0 (full) or T1 (small, since T0 is before its window) |
| `t_s` | number | The step in window time (300 full, 67 small) |
| `pattern`, `repoint` | string | `three`, `off` |
| `failed_satellites` | array of `sat` | The failure file's satellites |
| `failed_links` | array of `[sat, sat]` | The laser links the run's facts CSV reports down (the links of the failed satellites), in its order |
| `planned_links_not_failed` | array of integer | **(computed)** The planned laser links that did not fail: indices into `links-three.json` `links` (the run's laser plan is that one), every link not in `failed_links`, in file order |
| `routes` | array | One per study pair, one way: `from`, `to` (`site:city`), and `baseline` (no failure) and `reference` (after recovery, the planner knowing everything), each `{state, drop_cause, one_way_ms, route}` |
| `network` | array | The network CSV at the step: `scope` (`all` or a shell), `working_satellites`, `pieces`, `largest_piece` (integers), `largest_piece_share` (number or null) |

These are the planned links that did not fail, not the links that work: a planned link can
also be out of sight at an instant, and no program writes that. The page labels them so.

## The engine (`engine/`)

The routing program compiled to WebAssembly (`apps/routing_wasm`, built by `scripts/ci.sh`
into `build/wasm/bin`) and the inputs it reads, so the page can follow the route between any
two points with the route story's settings. `src/engine/run.ts` runs it and
`src/engine/from-csv.ts` turns its output into the route story's `directions`;
`scripts/check-engine.ts` holds both to the route story and to the native program.

| File | What |
|---|---|
| `engine/satsim-routing.js`, `engine/satsim-routing.wasm` | The program: an ES module whose default export makes a fresh instance (Emscripten's `MODULARIZE`), for a page, a Web Worker or Node, and its binary; copied from the build |
| `engine/snapshot/gp.json`, `supgp.json`, `finals2000A.all` | The snapshot's files (`data/starlink/<snapshot>/`), unchanged |
| `engine/ground/gateways.json`, `assumed_gateways.json`, `pop_codes.json`, `starlink_pops.csv`, `starlink_geofeed.csv` | The ground data's files (`data/ground/<ground>/`), unchanged |

The data check verifies every one byte for byte with its original. The manifest's `engine`:

| Field | Type | Meaning |
|---|---|---|
| `program` | object | `js`, `wasm`: the two program files' paths |
| `snapshot`, `ground` | array of string | The input files' paths, written into the program's in-memory file system as the directories `--snapshot` and `--ground` name |
| `settings` | object | The route story's: `model` (`best-case`), `pattern` (`three`), `replan_s` (15), `planner` (`start`), `backup` (`on`), `step_s` (1), `plan_time_utc` (the plan time), `roles` (`study`) |
| `strategies` | array of string | The strategies of a run between two chosen points: `lowest-delay`, `keep-until-broken`, `threshold:2ms` (longest-lived needs the most memory and time) |
| `window` | object | `start_utc`, `duration_s`: a run must lie inside it; the position window (`settings.window_start_utc`, `settings.window_s`) |
| `arguments` | array of string | The route story's `routing` arguments, with `{snapshot}`, `{ground}`, `{cities}` (a one-pair cities file), `{start_utc}`, `{duration_s}`, `{strategies}` (comma-separated) and `{drops}` (the drops CSV's path) in place of what a run chooses; `--out -` sends the routes CSV to the program's counted output |

The program reads its arguments from `callMain` and writes `/run/routes.csv` (counting its
lines into `Module.onRoutesLine`, for progress), `/run/summary.txt` and `/run/messages.txt`;
the exit status is the routing program's (0, 1 if a check failed, 2 for bad arguments or
inputs).

"Try it" (`src/stories/explore/`) runs it in a Web Worker (`src/engine/worker.ts`): the page
downloads every file of this block through the manifest (refusing a size that is not the
manifest's), compiles the `.wasm` once and hands the worker the program's text, the compiled
module and the inputs; the worker imports the program from a `blob:` URL, so the page works
from any directory (a GitHub Pages sub-path included). A run covers the window's start to at
most 600 s later, with `strategies`.

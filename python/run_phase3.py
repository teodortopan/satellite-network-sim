#!/usr/bin/env python3
"""Run every Phase 3 routing run on the real snapshot and write the committed summaries.

Takes the optimised `routing` program, builds nothing, and runs into an output directory that
is not committed (default results/phase-3/, ignored by git). Every run: 2026-11-18T21:15:00
for 2 h at 1 s steps (7,201 instants), the six study pairs in both directions, ground data of
2026-11-19, --backup on, the drops CSV written. The five groups of spec section 5:

  main      every strategy, both models; re-plan 15 s, planner start, pattern three, serving
            highest-elevation.
  interval  re-plan 1 s and 60 s, both models; lowest-delay and keep-until-broken.
  planner   planner whole-interval, both models; lowest-delay; re-plan 15 s.
  serving   serving longest-in-view, highest-whole-slot and with-route, realistic only; every
            strategy; re-plan 15 s.
  pattern   pattern plus-grid and six, both models; lowest-delay and keep-until-broken.

Every strategy: lowest-delay, keep-until-broken, longest-lived, threshold 0.5, 1, 2 and 5 ms,
threshold 1, 2, 5 and 10%. Each run writes <name>.csv.gz (the routes CSV, compressed),
<name>.drops.csv, <name>.summary.txt and <name>.status (exit status and wall time).

Every measure is recounted here from the routes CSV, by the definitions of spec 4.7: a path
change is two consecutive ok steps whose routes differ (best case the satellites; realistic
the satellites and the gateway, since the program compares node lists), bad when the delay
goes down and an increase otherwise, so a change with exactly equal delay is an increase, as
routing::classify_change counts it; a break is an ok step followed by a drop and a recovery the
reverse; a change's size is the new route's delay at its first step minus the old one's at its
last; a lifetime is a run of consecutive ok steps with one route, runs cut by the start or the
end counted; rates are per hour of (steps - 1) x step; the extra over the ideal is taken at ok
steps. A drop event is a run of consecutive dropped steps of one direction and strategy with
one cause. The backup: on every re-plan row that holds a route (plan kept or replaced) the
routes CSV gives backup_ms, or nothing when no backup exists; the program's extra is the backup
minus the held route's delay in the planning graph. That delay is the row's one_way_ms for both
planners: with `start` the planning graph is the step's graph, and with `whole-interval` it has
a subset of the re-plan step's edges with the re-plan step's delays (spec 4.3), so a route up in
it has the same delay in the step's graph. The cross-check below confirms it for every run.

The recount is then held against the program's own summary for every run, direction and
strategy: steps, ok steps, plans (kept, replaced, none), bad changes, increases, breaks,
recoveries, seconds without a route per cause, drop events (also against the drops CSV, event
by event), and the counts and quantiles (to the summary's printed precision) of change size,
lifetime, delay, stretch, extra over the ideal, backup and backup extra, and backups missing;
per direction the ideal, the distance and the lower bound; per run the rows written and the
drop events. Any difference fails the script and names the run, direction, strategy and measure.

Summaries under docs/phase-3-data/:

  runs.csv              one row per run: settings, exit status, wall time, times, rows, drop
                        events, every route-check and physical-check count, the planned laser
                        links by kind, laser link-steps up and down by reason.
  measures.csv          per run, direction and strategy: steps, changes (counts and per hour),
                        change size, route lifetime, delay, stretch, ideal, extra over the
                        ideal, time without a route by cause, drop events, laser hops, backup;
                        realistic: serving-satellite changes at slot starts (a slot start whose
                        serving satellite, or none, differs from the previous slot start's),
                        gateway changes (consecutive ok steps with different gateways) and the
                        gateways used with their ok-step counts.
  drops.csv             per run, direction, strategy and cause: drop events, seconds, median and
                        longest event.
  drop_offsets.csv      per run, strategy and cause (ground_link_lost, serving_left_view,
                        laser_link_down): drop events starting at each offset since the last
                        re-plan instant, 0 to replan_s - 1, summed over the directions.
  access_elevation.csv  the main runs and the best-case interval runs, strategies lowest-delay,
                        keep-until-broken and longest-lived: the elevation of the route's access
                        satellite at every re-plan where the plan was replaced (best case the
                        first satellite from the sending city and the last from the receiving
                        city; realistic the first, serving, satellite from the sending city),
                        against its mask. Positions from the `propagate` program (ITRF), the
                        city on the WGS-84 ellipsoid at height 0, elevation from the geodetic
                        normal as topology::look_angle computes it, and the FCC mask by the
                        shell's mean height (topology::shell_mean_heights_km: the mean of
                        mean_height_km over the shell's satellites in planes, from the
                        `constellation` program, which must sit next to `propagate`). Every
                        such elevation must be at or above its mask (to 1e-9 rad), since the
                        satellite was usable at that step; one that is not fails the script.
                        Written only with --propagate.

Percentiles are numpy's default (linear interpolation), as the program's. Exit status 1 if any
run fails or reports a route-check or physical-check failure, or a cross-check fails (nothing
is written then), 2 on a usage error.

Usage (inside the CI image; about 30 minutes with 4 parallel runs):

    python3 python/run_phase3.py --routing build/validate/bin/routing \\
        --propagate build/validate/bin/propagate

--reuse keeps the finished runs already in --out, so with every run there it rebuilds the
summaries without running the program. --duration-s shortens every run, for trials only; it
then needs a --data-dir other than the committed one.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import gzip
import itertools
import json
import math
import re
import shutil
import subprocess
import sys
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

import snapshot_tools as st

REPO = st.REPO_ROOT
SNAPSHOT = REPO / "data/starlink/2026-11-18"
GROUND = REPO / "data/ground/2026-11-19"
CITIES = REPO / "data/ground/cities.json"
DEFAULT_OUT = REPO / "results/phase-3"
DATA_DIR = REPO / "docs/phase-3-data"

START = "2026-11-18T21:15:00"
DURATION_S = 2 * 3600
STEP_S = 1
SLOT_S = 15
MODELS = ("realistic", "best-case")
ALL_STRATEGIES = (
    "lowest-delay",
    "keep-until-broken",
    "longest-lived",
    "threshold:0.5ms",
    "threshold:1ms",
    "threshold:2ms",
    "threshold:5ms",
    "threshold:1%",
    "threshold:2%",
    "threshold:5%",
    "threshold:10%",
)
TWO_STRATEGIES = ("lowest-delay", "keep-until-broken")
DROP_CAUSES = (  # The program's order (kAllDropCauses).
    "no_service",
    "no_coverage",
    "serving_left_view",
    "ground_link_lost",
    "laser_link_down",
    "no_gateway",
    "no_path",
)
OFFSET_CAUSES = ("ground_link_lost", "serving_left_view", "laser_link_down")
ELEVATION_STRATEGIES = ("lowest-delay", "keep-until-broken", "longest-lived")
ELEVATION_RUNS = (
    "main-realistic",
    "main-best-case",
    "interval-1s-best-case",
    "interval-60s-best-case",
)
NEAR_MASK_DEG = 5.0
ELEVATION_SLACK_RAD = 1e-9
PROPAGATE_WINDOW_S = 900

C_KM_S = 299_792.458
WGS84_A_KM = 6378.137
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = WGS84_F * (2.0 - WGS84_F)
MEAN_RADIUS_KM = (2 * WGS84_A_KM + WGS84_A_KM * (1.0 - WGS84_F)) / 3  # kEarthMeanRadiusKm.
RAD_PER_DEG = math.pi / 180.0
# FCC order DA 26-36, by the shell's mean height (topology/geometry.cpp).
LOW_TIER_CEILING_KM = 400.0
HIGH_TIER_FLOOR_KM = 500.0


@dataclass(frozen=True)
class Run:
    name: str
    group: str
    model: str
    strategies: tuple[str, ...]
    pattern: str = "three"
    replan_s: int = 15
    planner: str = "start"
    serving: str = "highest-elevation"  # Realistic only; "" in the best case.


def all_runs() -> list[Run]:
    def serving(model):
        return "highest-elevation" if model == "realistic" else ""

    runs = [Run(f"main-{m}", "main", m, ALL_STRATEGIES, serving=serving(m)) for m in MODELS]
    for replan in (1, 60):
        for m in MODELS:
            runs.append(
                Run(
                    f"interval-{replan}s-{m}",
                    "interval",
                    m,
                    TWO_STRATEGIES,
                    replan_s=replan,
                    serving=serving(m),
                )
            )
    for m in MODELS:
        runs.append(
            Run(
                f"planner-whole-interval-{m}",
                "planner",
                m,
                ("lowest-delay",),
                planner="whole-interval",
                serving=serving(m),
            )
        )
    for rule in ("longest-in-view", "highest-whole-slot", "with-route"):
        runs.append(Run(f"serving-{rule}", "serving", "realistic", ALL_STRATEGIES, serving=rule))
    for pattern in ("plus-grid", "six"):
        for m in MODELS:
            runs.append(
                Run(
                    f"pattern-{pattern}-{m}",
                    "pattern",
                    m,
                    TWO_STRATEGIES,
                    pattern=pattern,
                    serving=serving(m),
                )
            )
    return runs


# ---------------------------------------------------------------------------------------
# Running.
# ---------------------------------------------------------------------------------------
def command(run: Run, routing: Path, duration_s: int, csv_path: Path, drops_path: Path) -> list:
    serving = ["--serving", run.serving] if run.serving else []
    return [
        str(routing),
        "--snapshot",
        str(SNAPSHOT),
        "--ground",
        str(GROUND),
        "--cities",
        str(CITIES),
        "--start",
        START,
        "--duration-s",
        str(duration_s),
        "--step-s",
        str(STEP_S),
        "--pattern",
        run.pattern,
        "--model",
        run.model,
        "--roles",
        "study",
        "--strategies",
        ",".join(run.strategies),
        "--replan-s",
        str(run.replan_s),
        "--planner",
        run.planner,
        *serving,
        "--backup",
        "on",
        "--out",
        str(csv_path),
        "--drops-out",
        str(drops_path),
    ]


def execute(
    run: Run, routing: Path, duration_s: int, out: Path, reuse: bool
) -> tuple[Run, int, float]:
    gz_path = out / f"{run.name}.csv.gz"
    drops_path = out / f"{run.name}.drops.csv"
    summary_path = out / f"{run.name}.summary.txt"
    status_path = out / f"{run.name}.status"
    if reuse and all(p.is_file() for p in (gz_path, drops_path, summary_path, status_path)):
        status = json.loads(status_path.read_text())
        return run, status["exit"], status["wall_s"]
    csv_path = out / f"{run.name}.csv"
    began = time.monotonic()
    result = subprocess.run(
        command(run, routing, duration_s, csv_path, drops_path),
        capture_output=True,
        text=True,
        check=False,
    )
    elapsed = time.monotonic() - began
    summary_path.write_text(result.stdout + result.stderr)
    if csv_path.is_file():
        with csv_path.open("rb") as src, gzip.open(gz_path, "wb", compresslevel=5) as dst:
            shutil.copyfileobj(src, dst)
        csv_path.unlink()
    status_path.write_text(json.dumps({"exit": result.returncode, "wall_s": round(elapsed, 1)}))
    return run, result.returncode, elapsed


# ---------------------------------------------------------------------------------------
# The program's summary.
# ---------------------------------------------------------------------------------------
_ROUTE_CHECKS = re.compile(r"route-check failures \(must be 0\): (.*)$", re.M)
_CHECKS = re.compile(r"physical-check failures \(must be 0\): (.*)$", re.M)
_LINKS = re.compile(r"link-steps up: (\d+), down: (.*)$", re.M)
_PLANNED = re.compile(r"planned laser links: (\d+)\n((?:    [a-z_]+: \d+\n)*)")
_TIMES = re.compile(r"^  times: (\d+),", re.M)
_ROWS = re.compile(r"rows written: (\d+), drop events: (\d+)")
_DIRECTION = re.compile(r"^    (\S.*) -> (.*) \((\w+), (\d+) km, lower bound ([\d.]+) ms\)$")
_IDEAL = re.compile(r"^      ideal: (.*)$")
_STRATEGY = re.compile(
    r"^      (\S+): ok (\d+) of (\d+) steps; "
    r"plans (\d+) \(kept (\d+), replaced (\d+), none (\d+)\)$"
)
_MEASURE = re.compile(r"^        ([a-z ]+): (.*)$")
_COUNTS = re.compile(r"counts: bad (\d+), increases (\d+), breaks (\d+), recoveries (\d+)$")
_QUANTILES = re.compile(
    r"min (-?[\d.]+)[^,]*, p5 (-?[\d.]+)[^,]*, median (-?[\d.]+)[^,]*, "
    r"p95 (-?[\d.]+)[^,]*, max (-?[\d.]+)[^(]*\((\d+)\)"
)


def pairs_of(text: str) -> dict[str, int]:
    """'a 1, b 2' or 'a 1 b 2' -> {'a': 1, 'b': 2}."""
    return {k: int(v) for k, v in re.findall(r"([a-z_]+) (\d+)", text)}


def quantiles_of(text: str) -> tuple | None:
    """'min 1.0 ms, p5 ..., max 3.0 ms (7)' -> (min, p5, median, p95, max, count); 'none'."""
    text = text.strip()
    if text == "none":
        return None
    m = _QUANTILES.fullmatch(text)
    if not m:
        raise SystemExit(f"error: cannot read quantiles {text!r} in a summary")
    return (*(float(x) for x in m.groups()[:5]), int(m.group(6)))


def parse_summary(text: str) -> dict:
    route_checks = _ROUTE_CHECKS.search(text)
    checks = _CHECKS.search(text)
    links = _LINKS.search(text)
    planned = _PLANNED.search(text)
    times = _TIMES.search(text)
    rows = _ROWS.search(text)
    parsed = {
        "route_checks": pairs_of(route_checks.group(1)) if route_checks else None,
        "checks": pairs_of(checks.group(1)) if checks else None,
        "link_steps_up": int(links.group(1)) if links else None,
        "link_steps_down": pairs_of(links.group(2)) if links else {},
        "planned_links": int(planned.group(1)) if planned else None,
        "planned_by_kind": dict(re.findall(r"([a-z_]+): (\d+)", planned.group(2)))
        if planned
        else {},
        "times": int(times.group(1)) if times else None,
        "rows_written": int(rows.group(1)) if rows else None,
        "drop_events": int(rows.group(2)) if rows else None,
        "passed": "checks: PASSED" in text,
        "directions": {},  # (from, to) -> {"km", "lower_ms", "ideal", "strategies": {...}}
    }
    direction = lane = None
    for line in text.splitlines():
        if m := _DIRECTION.match(line):
            direction = {
                "km": int(m.group(4)),
                "lower_ms": float(m.group(5)),
                "ideal": None,
                "strategies": {},
            }
            parsed["directions"][(m.group(1), m.group(2))] = direction
            lane = None
        elif direction is not None and (m := _IDEAL.match(line)):
            direction["ideal"] = quantiles_of(m.group(1))
        elif direction is not None and (m := _STRATEGY.match(line)):
            names = ("ok", "steps", "plans", "kept", "replaced", "none")
            lane = dict(zip(names, (int(x) for x in m.groups()[1:]), strict=True))
            direction["strategies"][m.group(1)] = lane
        elif lane is not None and (m := _MEASURE.match(line)):
            what, value = m.groups()
            if what == "path changes per hour":
                c = _COUNTS.search(value)
                names = ("bad", "increases", "breaks", "recoveries")
                lane.update(zip(names, map(int, c.groups()), strict=True))
            elif what == "no route":
                causes, events = value.rsplit("; drop events ", 1)
                found = re.findall(r"([a-z_]+) ([\d.]+) s", causes)
                lane["drop_s"] = {k: float(v) for k, v in found}
                lane["drop_events"] = int(events)
            elif what == "backup":
                backup, extra, missing = value.split("; ")
                lane["backup"] = quantiles_of(backup)
                lane["backup extra"] = quantiles_of(extra.removeprefix("extra "))
                lane["backup missing"] = int(re.fullmatch(r"none at (\d+) plans", missing).group(1))
            else:
                lane[what] = quantiles_of(value)
    return parsed


# ---------------------------------------------------------------------------------------
# The recount, one direction and strategy at a time.
# ---------------------------------------------------------------------------------------
def optional_float(text: str) -> float | None:
    return float(text) if text else None


@dataclass
class Lane:
    """The measures of one direction and strategy, accumulated step by step."""

    model: str
    steps: int = 0
    ok_steps: int = 0
    plans: Counter = field(default_factory=Counter)
    counts: Counter = field(default_factory=Counter)  # bad, increases, breaks, recoveries
    change_sizes_ms: list = field(default_factory=list)
    lifetimes_s: list = field(default_factory=list)
    delay_ms: list = field(default_factory=list)
    extra_ms: list = field(default_factory=list)
    ideal_ms: list = field(default_factory=list)
    hops: list = field(default_factory=list)
    drop_steps: Counter = field(default_factory=Counter)
    events: list = field(default_factory=list)  # [start step, steps, cause]
    backup_ms: list = field(default_factory=list)
    backup_extra_ms: list = field(default_factory=list)
    backup_missing: int = 0
    slot_serving: list = field(default_factory=list)  # serving_norad_cat_id at slot starts
    gateway_changes: int = 0
    gateways: Counter = field(default_factory=Counter)
    replaced: list = field(default_factory=list)  # (step, first satellite, last satellite)
    anomalies: list = field(default_factory=list)
    previous: tuple | None = None  # (ok, route key, delay ms, gateway)
    life_steps: int = 0
    open_event: list | None = None

    def close_event(self) -> None:
        if self.open_event is not None:
            self.events.append(self.open_event)
            self.open_event = None

    def add(self, k: int, r: dict) -> None:
        self.steps += 1
        ok = r["status"] == "ok"
        cause = r["drop_cause"]
        delay = optional_float(r["one_way_ms"])
        ideal = optional_float(r["ideal_ms"])
        gateway = f"{r['gateway']} ({r['gateway_source']})" if r["gateway"] else ""
        key = (r["route"], gateway)
        plan = r["plan"]
        if ideal is not None:
            self.ideal_ms.append(ideal)
        if plan:
            self.plans[plan] += 1
        if ok:
            self.ok_steps += 1
            self.delay_ms.append(delay)
            if ideal is not None:
                self.extra_ms.append(delay - ideal)
            self.hops.append(int(r["laser_hops"]))
            if gateway:
                self.gateways[gateway] += 1
        elif cause:
            self.drop_steps[cause] += 1

        previous = self.previous
        if previous is not None:
            was_ok, was_key, was_delay, was_gateway = previous
            if was_ok and ok:
                if was_key != key:
                    self.counts["bad" if delay < was_delay else "increases"] += 1
                    self.change_sizes_ms.append(delay - was_delay)
                if was_gateway != gateway:
                    self.gateway_changes += 1
            elif was_ok:
                self.counts["breaks"] += 1
            elif ok:
                self.counts["recoveries"] += 1
        if ok:
            if previous is not None and previous[0] and previous[1] == key:
                self.life_steps += 1
            else:
                if self.life_steps:
                    self.lifetimes_s.append(self.life_steps * STEP_S)
                self.life_steps = 1
        else:
            if self.life_steps:
                self.lifetimes_s.append(self.life_steps * STEP_S)
            self.life_steps = 0
        if not ok and cause:
            if self.open_event is not None and self.open_event[2] == cause:
                self.open_event[1] += 1
            else:
                self.close_event()
                self.open_event = [k, 1, cause]
        else:
            self.close_event()

        if plan in ("kept", "replaced"):
            if not ok:
                self.anomalies.append(f"step {k}: plan {plan} on a dropped step")
            elif r["backup_ms"]:
                self.backup_ms.append(float(r["backup_ms"]))
                self.backup_extra_ms.append(float(r["backup_ms"]) - delay)
            else:
                self.backup_missing += 1
            if plan == "replaced" and ok:
                self.replaced.append((k, int(r["first_norad_cat_id"]), int(r["last_norad_cat_id"])))
        if self.model == "realistic" and (k * STEP_S) % SLOT_S == 0:
            self.slot_serving.append(r["serving_norad_cat_id"])
        self.previous = (ok, key, delay, gateway)

    def finish(self) -> None:
        self.close_event()
        if self.life_steps:
            self.lifetimes_s.append(self.life_steps * STEP_S)
            self.life_steps = 0

    def serving_changes(self) -> int:
        return sum(1 for a, b in itertools.pairwise(self.slot_serving) if a != b)


def read_lanes(out: Path, run: Run) -> tuple[list[tuple[str, str]], dict, int]:
    """The routes CSV of a run, streamed into one Lane per (from, to, strategy). Returns the
    directions in the program's order, the lanes and the number of rows."""
    start_ns = st.parse_utc(START).unix_ns
    directions: list[tuple[str, str]] = []
    lanes: dict[tuple[str, str, str], Lane] = {}
    last_utc, k, count = None, 0, 0
    with gzip.open(out / f"{run.name}.csv.gz", "rt", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            count += 1
            if r["utc"] != last_utc:
                last_utc = r["utc"]
                k = (st.parse_utc(last_utc).unix_ns - start_ns) // (STEP_S * st.NS_PER_S)
            if r["strategy"] not in run.strategies:
                raise SystemExit(f"error: {run.name}: unexpected strategy {r['strategy']!r}")
            direction = (r["from"], r["to"])
            lane_key = (*direction, r["strategy"])
            lane = lanes.get(lane_key)
            if lane is None:
                if direction not in directions:
                    directions.append(direction)
                lane = lanes[lane_key] = Lane(run.model)
            lane.add(k, r)
    for lane in lanes.values():
        lane.finish()
    return directions, lanes, count


def read_drops(path: Path) -> list[dict]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


# ---------------------------------------------------------------------------------------
# Numbers and text.
# ---------------------------------------------------------------------------------------
def fmt(value, decimals: int) -> str:
    return "" if value is None else f"{value:.{decimals}f}"


def pct(values: list, p: float) -> float | None:
    return float(np.percentile(values, p)) if len(values) else None


def median(values: list) -> float | None:
    return float(np.median(values)) if len(values) else None


def maximum(values: list) -> float | None:
    return float(max(values)) if len(values) else None


def minimum(values: list) -> float | None:
    return float(min(values)) if len(values) else None


def quantiles(values: list) -> tuple | None:
    """As the program's app::quantiles: (min, p5, median, p95, max, count), or None."""
    if not len(values):
        return None
    a = np.asarray(values, dtype=float)
    return (
        float(a.min()),
        float(np.percentile(a, 5)),
        float(np.median(a)),
        float(np.percentile(a, 95)),
        float(a.max()),
        len(a),
    )


def counts_text(counter: Counter) -> str:
    return "; ".join(f"{k}: {v}" for k, v in sorted(counter.items(), key=lambda x: (-x[1], x[0])))


def great_circle_km(lat1_deg, lon1_deg, lat2_deg, lon2_deg) -> float:
    """ground::great_circle_distance_on_mean_sphere_km, term for term."""
    p1, l1, p2, l2 = (x * RAD_PER_DEG for x in (lat1_deg, lon1_deg, lat2_deg, lon2_deg))
    dl = l2 - l1
    sin_half = math.sin(dl / 2.0)
    y = math.hypot(
        math.cos(p2) * math.sin(dl),
        math.sin(p2 - p1) + 2.0 * math.sin(p1) * math.cos(p2) * sin_half * sin_half,
    )
    x = math.sin(p1) * math.sin(p2) + math.cos(p1) * math.cos(p2) * math.cos(dl)
    return MEAN_RADIUS_KM * math.atan2(y, x)


def load_cities() -> dict[str, tuple[float, float]]:
    cities = json.loads(CITIES.read_text())["cities"]
    return {c["name"]: (c["lat"], c["lon"]) for c in cities}


# ---------------------------------------------------------------------------------------
# The cross-check against the program's summary and drops CSV.
# ---------------------------------------------------------------------------------------
def cross_check(
    run: Run,
    parsed: dict,
    directions: list,
    lanes: dict,
    rows: int,
    drops: list[dict],
    distance_km: dict,
) -> list[str]:
    bad = []

    def differ(where: str, what: str, mine, theirs) -> None:
        bad.append(f"{run.name}: {where}: {what}: recount {mine}, program {theirs}")

    def same_quantiles(where: str, what: str, mine: tuple | None, theirs: tuple | None, dec: int):
        if (mine is None) != (theirs is None):
            differ(where, what, mine, theirs)
            return
        if mine is None:
            return
        tolerance = 0.5 * 10.0**-dec + 1e-6
        pairs = zip(mine[:5], theirs[:5], strict=True)
        if mine[5] != theirs[5] or any(abs(a - b) > tolerance for a, b in pairs):
            differ(where, what, tuple(round(x, dec + 2) for x in mine[:5]) + mine[5:], theirs)

    if rows != parsed["rows_written"]:
        differ("run", "rows written", rows, parsed["rows_written"])
    if len(drops) != parsed["drop_events"]:
        differ("run", "drop events in the drops CSV", len(drops), parsed["drop_events"])
    if list(parsed["directions"]) != directions:
        differ("run", "directions", directions, list(parsed["directions"]))
    start_ns = st.parse_utc(START).unix_ns
    program_events: dict[tuple, list] = {}
    for e in drops:
        k = (st.parse_utc(e["start_utc"]).unix_ns - start_ns) // (STEP_S * st.NS_PER_S)
        steps = float(e["length_s"]) / STEP_S
        program_events.setdefault((e["from"], e["to"], e["strategy"]), []).append(
            [k, steps, e["cause"]]
        )
    for direction in directions:
        theirs_d = parsed["directions"].get(direction)
        where_d = f"{direction[0]} -> {direction[1]}"
        if theirs_d is None:
            differ(where_d, "direction", "present", "missing")
            continue
        km = distance_km[direction]
        if round(km) != theirs_d["km"]:
            differ(where_d, "distance km", km, theirs_d["km"])
        if abs(km / C_KM_S * 1e3 - theirs_d["lower_ms"]) > 0.0005 + 1e-9:
            differ(where_d, "lower bound ms", km / C_KM_S * 1e3, theirs_d["lower_ms"])
        first = lanes[(*direction, run.strategies[0])]
        same_quantiles(where_d, "ideal", quantiles(first.ideal_ms), theirs_d["ideal"], 3)
        for strategy in run.strategies:
            where = f"{where_d} {strategy}"
            lane = lanes.get((*direction, strategy))
            theirs = theirs_d["strategies"].get(strategy)
            if lane is None or theirs is None:
                differ(where, "strategy", lane is not None, theirs is not None)
                continue
            for what, mine in (
                ("ok", lane.ok_steps),
                ("steps", lane.steps),
                ("plans", sum(lane.plans.values())),
                ("kept", lane.plans["kept"]),
                ("replaced", lane.plans["replaced"]),
                ("none", lane.plans["none"]),
                ("bad", lane.counts["bad"]),
                ("increases", lane.counts["increases"]),
                ("breaks", lane.counts["breaks"]),
                ("recoveries", lane.counts["recoveries"]),
                ("drop_events", len(lane.events)),
                ("backup missing", lane.backup_missing),
            ):
                if mine != theirs.get(what):
                    differ(where, what, mine, theirs.get(what))
            mine_drop_s = {c: round(n * STEP_S, 1) for c, n in lane.drop_steps.items() if n}
            if mine_drop_s != theirs["drop_s"]:
                differ(where, "seconds without a route by cause", mine_drop_s, theirs["drop_s"])
            if program_events.get((*direction, strategy), []) != lane.events:
                differ(
                    where,
                    "drop events in the drops CSV (start step, steps, cause)",
                    lane.events,
                    program_events.get((*direction, strategy), []),
                )
            stretch = [d / (km / C_KM_S * 1e3) for d in lane.delay_ms]
            for what, values, dec in (
                ("change size", lane.change_sizes_ms, 3),
                ("route lifetime", lane.lifetimes_s, 1),
                ("delay", lane.delay_ms, 3),
                ("stretch", stretch, 3),
                ("extra over the ideal", lane.extra_ms, 3),
                ("backup", lane.backup_ms, 3),
                ("backup extra", lane.backup_extra_ms, 3),
            ):
                same_quantiles(where, what, quantiles(values), theirs.get(what), dec)
            for anomaly in lane.anomalies:
                bad.append(f"{run.name}: {where}: {anomaly}")
    return bad


# ---------------------------------------------------------------------------------------
# The committed summaries.
# ---------------------------------------------------------------------------------------
def write_csv(path: Path, columns: list[str], rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=columns, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


def run_row(run: Run, duration_s: int, status: dict, parsed: dict) -> dict:
    return {
        "run": run.name,
        "group": run.group,
        "pattern": run.pattern,
        "model": run.model,
        "strategies": " ".join(run.strategies),
        "replan_s": run.replan_s,
        "planner": run.planner,
        "serving": run.serving,
        "duration_s": duration_s,
        "exit_status": status["exit"],
        "wall_s": status["wall_s"],
        "times": parsed["times"],
        "rows_written": parsed["rows_written"],
        "drop_events": parsed["drop_events"],
        **{f"route_check_{k}": v for k, v in (parsed["route_checks"] or {}).items()},
        **{f"physical_check_{k}": v for k, v in (parsed["checks"] or {}).items()},
        "planned_links": parsed["planned_links"],
        **{f"planned_{k}": v for k, v in parsed["planned_by_kind"].items()},
        "link_steps_up": parsed["link_steps_up"],
        **{f"down_{k}": v for k, v in parsed["link_steps_down"].items()},
    }


ID_COLUMNS = ["group", "run", "pattern", "model", "replan_s", "planner", "serving", "from", "to"]
MEASURE_COLUMNS = [
    *ID_COLUMNS,
    "strategy",
    "distance_km",
    "lower_bound_ms",
    "steps",
    "ok_steps",
    "ok_fraction",
    "plans",
    "plans_kept",
    "plans_replaced",
    "plans_none",
    "changes",
    "bad",
    "increases",
    "breaks",
    "recoveries",
    "changes_per_h",
    "bad_per_h",
    "increases_per_h",
    "breaks_per_h",
    "change_min_ms",
    "change_median_ms",
    "change_max_ms",
    "change_abs_median_ms",
    "lifetime_count",
    "lifetime_p5_s",
    "lifetime_p25_s",
    "lifetime_median_s",
    "lifetime_p75_s",
    "lifetime_p95_s",
    "lifetime_max_s",
    "delay_min_ms",
    "delay_median_ms",
    "delay_p95_ms",
    "delay_max_ms",
    "stretch_median",
    "stretch_p95",
    "ideal_median_ms",
    "extra_median_ms",
    "extra_p95_ms",
    "extra_max_ms",
    "no_route_s",
    *(f"no_route_{c}_s" for c in DROP_CAUSES),
    "drop_events",
    "longest_drop_s",
    "laser_hops_median",
    "laser_hops_max",
    "backup_plans",
    "backup_missing_plans",
    "backup_median_ms",
    "backup_minus_held_median_ms",
    "serving_changes",
    "slot_starts",
    "gateway_changes",
    "gateways",
]


def id_values(run: Run, direction: tuple[str, str]) -> dict:
    return {
        "group": run.group,
        "run": run.name,
        "pattern": run.pattern,
        "model": run.model,
        "replan_s": run.replan_s,
        "planner": run.planner,
        "serving": run.serving,
        "from": direction[0],
        "to": direction[1],
    }


def measure_row(run: Run, direction, strategy: str, lane: Lane, km: float, hours: float) -> dict:
    lower_ms = km / C_KM_S * 1e3
    changes = lane.counts["bad"] + lane.counts["increases"]
    stretch = [d / lower_ms for d in lane.delay_ms]
    event_s = [e[1] * STEP_S for e in lane.events]
    realistic = run.model == "realistic"

    def per_hour(n):
        return fmt(n / hours, 2) if hours > 0 else ""

    return {
        **id_values(run, direction),
        "strategy": strategy,
        "distance_km": fmt(km, 1),
        "lower_bound_ms": fmt(lower_ms, 3),
        "steps": lane.steps,
        "ok_steps": lane.ok_steps,
        "ok_fraction": fmt(lane.ok_steps / lane.steps, 4),
        "plans": sum(lane.plans.values()),
        "plans_kept": lane.plans["kept"],
        "plans_replaced": lane.plans["replaced"],
        "plans_none": lane.plans["none"],
        "changes": changes,
        "bad": lane.counts["bad"],
        "increases": lane.counts["increases"],
        "breaks": lane.counts["breaks"],
        "recoveries": lane.counts["recoveries"],
        "changes_per_h": per_hour(changes),
        "bad_per_h": per_hour(lane.counts["bad"]),
        "increases_per_h": per_hour(lane.counts["increases"]),
        "breaks_per_h": per_hour(lane.counts["breaks"]),
        "change_min_ms": fmt(minimum(lane.change_sizes_ms), 3),
        "change_median_ms": fmt(median(lane.change_sizes_ms), 3),
        "change_max_ms": fmt(maximum(lane.change_sizes_ms), 3),
        "change_abs_median_ms": fmt(median([abs(x) for x in lane.change_sizes_ms]), 3),
        "lifetime_count": len(lane.lifetimes_s),
        "lifetime_p5_s": fmt(pct(lane.lifetimes_s, 5), 1),
        "lifetime_p25_s": fmt(pct(lane.lifetimes_s, 25), 1),
        "lifetime_median_s": fmt(median(lane.lifetimes_s), 1),
        "lifetime_p75_s": fmt(pct(lane.lifetimes_s, 75), 1),
        "lifetime_p95_s": fmt(pct(lane.lifetimes_s, 95), 1),
        "lifetime_max_s": fmt(maximum(lane.lifetimes_s), 1),
        "delay_min_ms": fmt(minimum(lane.delay_ms), 3),
        "delay_median_ms": fmt(median(lane.delay_ms), 3),
        "delay_p95_ms": fmt(pct(lane.delay_ms, 95), 3),
        "delay_max_ms": fmt(maximum(lane.delay_ms), 3),
        "stretch_median": fmt(median(stretch), 3),
        "stretch_p95": fmt(pct(stretch, 95), 3),
        "ideal_median_ms": fmt(median(lane.ideal_ms), 3),
        "extra_median_ms": fmt(median(lane.extra_ms), 3),
        "extra_p95_ms": fmt(pct(lane.extra_ms, 95), 3),
        "extra_max_ms": fmt(maximum(lane.extra_ms), 3),
        "no_route_s": sum(lane.drop_steps.values()) * STEP_S,
        **{f"no_route_{c}_s": lane.drop_steps[c] * STEP_S for c in DROP_CAUSES},
        "drop_events": len(lane.events),
        "longest_drop_s": max(event_s) if event_s else 0,
        "laser_hops_median": fmt(median(lane.hops), 1),
        "laser_hops_max": max(lane.hops) if lane.hops else "",
        "backup_plans": len(lane.backup_ms),
        "backup_missing_plans": lane.backup_missing,
        "backup_median_ms": fmt(median(lane.backup_ms), 3),
        "backup_minus_held_median_ms": fmt(median(lane.backup_extra_ms), 3),
        "serving_changes": lane.serving_changes() if realistic else "",
        "slot_starts": len(lane.slot_serving) if realistic else "",
        "gateway_changes": lane.gateway_changes if realistic else "",
        "gateways": counts_text(lane.gateways) if realistic else "",
    }


DROP_COLUMNS = [
    *ID_COLUMNS,
    "strategy",
    "cause",
    "events",
    "total_s",
    "median_event_s",
    "longest_event_s",
]


def drop_rows(run: Run, direction, strategy: str, lane: Lane) -> list[dict]:
    rows = []
    for cause in DROP_CAUSES:
        lengths = [e[1] * STEP_S for e in lane.events if e[2] == cause]
        if lengths:
            rows.append(
                {
                    **id_values(run, direction),
                    "strategy": strategy,
                    "cause": cause,
                    "events": len(lengths),
                    "total_s": sum(lengths),
                    "median_event_s": fmt(median(lengths), 1),
                    "longest_event_s": max(lengths),
                }
            )
    return rows


OFFSET_COLUMNS = ["run", "group", "model", "replan_s", "strategy", "cause", "offset_s", "events"]


def offset_rows(run: Run, directions: list, lanes: dict) -> list[dict]:
    rows = []
    for strategy in run.strategies:
        for cause in OFFSET_CAUSES:
            counts = Counter()
            for direction in directions:
                for k, _, c in lanes[(*direction, strategy)].events:
                    if c == cause:
                        counts[(k * STEP_S) % run.replan_s] += 1
            for offset in range(run.replan_s):
                rows.append(
                    {
                        "run": run.name,
                        "group": run.group,
                        "model": run.model,
                        "replan_s": run.replan_s,
                        "strategy": strategy,
                        "cause": cause,
                        "offset_s": offset,
                        "events": counts[offset],
                    }
                )
    return rows


# ---------------------------------------------------------------------------------------
# Elevation of the access satellites at replaced plans.
# ---------------------------------------------------------------------------------------
def site_ecef_km(lat_deg: float, lon_deg: float) -> tuple[np.ndarray, np.ndarray]:
    """frames::geodetic_to_ecef at height 0, and the geodetic normal (topology::site_geometry)."""
    lat, lon = lat_deg * RAD_PER_DEG, lon_deg * RAD_PER_DEG
    n = WGS84_A_KM / math.sqrt(1.0 - WGS84_E2 * math.sin(lat) ** 2)
    position = np.array(
        [
            n * math.cos(lat) * math.cos(lon),
            n * math.cos(lat) * math.sin(lon),
            n * (1.0 - WGS84_E2) * math.sin(lat),
        ]
    )
    up = np.array([math.cos(lat) * math.cos(lon), math.cos(lat) * math.sin(lon), math.sin(lat)])
    return position, up


def elevation_rad(site: tuple[np.ndarray, np.ndarray], satellite_ecef_km: np.ndarray) -> float:
    """topology::look_angle: atan2 of the component along the normal and the rest."""
    position, up = site
    d = satellite_ecef_km - position
    along = float(up @ d)
    return math.atan2(along, float(np.linalg.norm(d - along * up)))


def mask_rad(height_km: float) -> float:
    """topology::elevation_mask_rad without overrides (none of these runs sets one)."""
    if height_km < LOW_TIER_CEILING_KM:
        return 10.0 * RAD_PER_DEG
    if height_km <= HIGH_TIER_FLOOR_KM:
        return 20.0 * RAD_PER_DEG
    return 25.0 * RAD_PER_DEG


def satellite_masks(constellation: Path, out: Path) -> dict[int, float]:
    """Each satellite in a plane -> the mask of its shell's mean height."""
    csv_path = out / "constellation.csv"
    result = subprocess.run(
        [
            str(constellation),
            "--snapshot",
            str(SNAPSHOT),
            "--time",
            START,
            "--out",
            str(csv_path),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(f"error: constellation exited with {result.returncode}\n{result.stderr}")
    with csv_path.open(newline="", encoding="utf-8") as f:
        members = [r for r in csv.DictReader(f) if r["plane_index"] and r["mean_height_km"]]
    csv_path.unlink()
    heights: dict[str, list[float]] = {}
    for r in members:
        heights.setdefault(r["shell"], []).append(float(r["mean_height_km"]))
    shell_mask = {s: mask_rad(sum(h) / len(h)) for s, h in heights.items()}
    return {int(r["norad_cat_id"]): shell_mask[r["shell"]] for r in members}


def positions_at(propagate: Path, out: Path, wanted: set[tuple[int, int]], step_s: int) -> dict:
    """(step, catalogue number) -> ITRF position, km, from the propagate program, in windows
    of PROPAGATE_WINDOW_S with only the satellites each window needs."""
    start = st.parse_utc(START)
    windows: dict[int, set] = {}
    for k, satellite in wanted:
        windows.setdefault(k * STEP_S // PROPAGATE_WINDOW_S, set()).add((k, satellite))
    found = {}
    csv_path = out / "propagate.csv"
    for _, needed in sorted(windows.items()):
        first = min(k for k, _ in needed) * STEP_S
        last = max(k for k, _ in needed) * STEP_S
        st.run_propagate(
            propagate,
            SNAPSHOT,
            start.plus_ns(first * st.NS_PER_S),
            (last - first) * st.NS_PER_S,
            step_s * st.NS_PER_S,
            sorted({s for _, s in needed}),
            csv_path,
        )
        for r in st.read_propagate_csv(csv_path):
            k = (st.parse_utc(r["utc"]).unix_ns - start.unix_ns) // (STEP_S * st.NS_PER_S)
            if (k, r["norad_cat_id"]) in needed:
                found[(k, r["norad_cat_id"])] = np.array(
                    [r["ecef_x_km"], r["ecef_y_km"], r["ecef_z_km"]]
                )
        csv_path.unlink()
    missing = wanted - set(found)
    if missing:
        raise SystemExit(f"error: propagate gave no position for {len(missing)} satellite-steps")
    return found


ELEVATION_COLUMNS = [
    *ID_COLUMNS,
    "strategy",
    "end",
    "plans_replaced",
    "elevation_min_deg",
    "elevation_p5_deg",
    "elevation_median_deg",
    "elevation_p95_deg",
    "above_mask_median_deg",
    "near_mask_fraction",
]


def elevation_rows(
    run: Run, directions: list, lanes: dict, propagate: Path, masks: dict, out: Path
) -> tuple[list[dict], list[str]]:
    cities = load_cities()
    sites = {name: site_ecef_km(*cities[name]) for d in directions for name in d}
    ends = [("first", 1)] if run.model == "realistic" else [("first", 1), ("last", 2)]
    strategies = [s for s in ELEVATION_STRATEGIES if s in run.strategies]
    wanted = set()
    for direction in directions:
        for strategy in strategies:
            for plan in lanes[(*direction, strategy)].replaced:
                for _, i in ends:
                    wanted.add((plan[0], plan[i]))
    positions = positions_at(propagate, out, wanted, run.replan_s)
    rows, bad = [], []
    for direction in directions:
        for strategy in strategies:
            for end, i in ends:
                city = direction[0] if end == "first" else direction[1]
                elevations, margins = [], []
                for plan in lanes[(*direction, strategy)].replaced:
                    e = elevation_rad(sites[city], positions[(plan[0], plan[i])])
                    mask = masks[plan[i]]
                    if e < mask - ELEVATION_SLACK_RAD:
                        bad.append(
                            f"{run.name}: {direction[0]} -> {direction[1]} {strategy}: step "
                            f"{plan[0]}: satellite {plan[i]} at {e / RAD_PER_DEG:.6f} deg from "
                            f"{city}, below its mask {mask / RAD_PER_DEG:.1f} deg"
                        )
                    elevations.append(e / RAD_PER_DEG)
                    margins.append((e - mask) / RAD_PER_DEG)
                near = sum(1 for m in margins if m < NEAR_MASK_DEG)
                rows.append(
                    {
                        **id_values(run, direction),
                        "strategy": strategy,
                        "end": end,
                        "plans_replaced": len(elevations),
                        "elevation_min_deg": fmt(minimum(elevations), 3),
                        "elevation_p5_deg": fmt(pct(elevations, 5), 3),
                        "elevation_median_deg": fmt(median(elevations), 3),
                        "elevation_p95_deg": fmt(pct(elevations, 95), 3),
                        "above_mask_median_deg": fmt(median(margins), 3),
                        "near_mask_fraction": fmt(near / len(margins), 4) if margins else "",
                    }
                )
    return rows, bad


# ---------------------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run the Phase 3 routing runs.")
    parser.add_argument("--routing", type=Path, required=True)
    parser.add_argument("--propagate", type=Path, help="for access_elevation.csv (optional)")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--data-dir", type=Path, default=DATA_DIR, help="summary directory")
    parser.add_argument("--jobs", type=int, default=4, help="runs at the same time")
    parser.add_argument("--reuse", action="store_true", help="keep finished runs already in --out")
    parser.add_argument(
        "--duration-s", type=int, default=DURATION_S, help="shorter runs, for trials only"
    )
    args = parser.parse_args(argv)
    if not args.routing.is_file():
        print(f"error: {args.routing} not found", file=sys.stderr)
        return 2
    constellation = args.propagate.parent / "constellation" if args.propagate else None
    for exe in (args.propagate, constellation):
        if exe is not None and not exe.is_file():
            print(f"error: {exe} not found", file=sys.stderr)
            return 2
    if args.duration_s <= 0 or args.duration_s > DURATION_S:
        print(f"error: --duration-s must be in 1 .. {DURATION_S}", file=sys.stderr)
        return 2
    if args.duration_s != DURATION_S and args.data_dir.resolve() == DATA_DIR.resolve():
        print("error: a trial (--duration-s) needs a --data-dir of its own", file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)

    runs = all_runs()
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [
            pool.submit(execute, r, args.routing, args.duration_s, args.out, args.reuse)
            for r in runs
        ]
        for future in concurrent.futures.as_completed(futures):
            run, status, elapsed = future.result()
            print(f"{run.name}: exit {status} ({elapsed:.0f} s)", flush=True)
    statuses, summaries = {}, {}
    for run in runs:
        statuses[run.name] = json.loads((args.out / f"{run.name}.status").read_text())
        parsed = summaries[run.name] = parse_summary(
            (args.out / f"{run.name}.summary.txt").read_text()
        )
        route_total = sum(parsed["route_checks"].values()) if parsed["route_checks"] else None
        check_total = sum(parsed["checks"].values()) if parsed["checks"] else None
        status = statuses[run.name]["exit"]
        if status != 0 or not parsed["passed"] or route_total != 0 or check_total != 0:
            failures.append(
                f"{run.name}: exit {status}, route checks {parsed['route_checks']}, "
                f"physical checks {parsed['checks']}"
            )
    if failures:
        print("FAIL:", *failures, sep="\n  ", file=sys.stderr)
        return 1

    cities = load_cities()
    masks = satellite_masks(constellation, args.out) if args.propagate else None
    hours = args.duration_s / 3600.0  # (steps - 1) x step.
    run_rows, measures, drops, offsets, elevations = [], [], [], [], []
    for run in runs:
        parsed = summaries[run.name]
        directions, lanes, count = read_lanes(args.out, run)
        distance_km = {d: great_circle_km(*cities[d[0]], *cities[d[1]]) for d in directions}
        program_drops = read_drops(args.out / f"{run.name}.drops.csv")
        mismatches = cross_check(run, parsed, directions, lanes, count, program_drops, distance_km)
        if parsed["times"] != args.duration_s // STEP_S + 1:
            mismatches.append(f"{run.name}: times {parsed['times']}")
        failures += mismatches
        print(f"{run.name}: {count} rows, {len(mismatches)} cross-check differences", flush=True)
        run_rows.append(run_row(run, args.duration_s, statuses[run.name], parsed))
        for direction in directions:
            for strategy in run.strategies:
                lane = lanes[(*direction, strategy)]
                measures.append(
                    measure_row(run, direction, strategy, lane, distance_km[direction], hours)
                )
                drops += drop_rows(run, direction, strategy, lane)
        offsets += offset_rows(run, directions, lanes)
        if masks is not None and run.name in ELEVATION_RUNS:
            rows, bad = elevation_rows(run, directions, lanes, args.propagate, masks, args.out)
            elevations += rows
            failures += bad
    if failures:
        print("FAIL:", *failures[:200], sep="\n  ", file=sys.stderr)
        if len(failures) > 200:
            print(f"  ... and {len(failures) - 200} more", file=sys.stderr)
        return 1

    columns = list(run_rows[0])
    for row in run_rows:
        columns += [k for k in row if k not in columns]
    outputs = [
        ("runs.csv", columns, run_rows),
        ("measures.csv", MEASURE_COLUMNS, measures),
        ("drops.csv", DROP_COLUMNS, drops),
        ("drop_offsets.csv", OFFSET_COLUMNS, offsets),
    ]
    if masks is not None:
        outputs.append(("access_elevation.csv", ELEVATION_COLUMNS, elevations))
    else:
        print("no --propagate: access_elevation.csv not written")
    for name, cols, rows in outputs:
        write_csv(args.data_dir / name, cols, rows)
        print(f"wrote {args.data_dir / name} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

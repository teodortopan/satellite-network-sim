#!/usr/bin/env python3
"""Run every Phase 4 failure run on the real snapshot and write the committed summaries.

Takes the optimised `failures` program (and `constellation` and `propagate`, which must sit
next to it), builds nothing, and runs into an output directory that is not committed (default
results/phase-4/, ignored by git). The groups are the two tables of docs/phase-4-failures.md
section 5. Fixed in every run: snapshot data/starlink/2026-11-18, ground data/ground/2026-11-19,
--plan-time 2026-11-18T21:15:00, 1 s steps, strategy lowest-delay, --planner whole-interval
(unless a group varies it), --replan-s 15, pattern three (unless a group varies it), serving
highest-elevation (realistic), --directions forward, and the times of spec 4.9 at their
defaults unless a group varies one: --detection-s 0.1, --hop-processing-s 0.001, --compute-s
0.1, --planner-processing-s 0, --acquisition-s 30, --repoint off, --replan-on-report off. Every
one of these is passed explicitly.

The failure instants: T_i = 21:15:00 + 300 s + i x 697 s, i = 0 to 9. A hand-picked run starts
at the last re-plan instant (21:15:00 + a multiple of 15 s) at least 60 s before T_i and ends
300 s after T_i.

Hand-picked groups: one run per (group, scenario, variant, model, pair, instant), carrying only
the affected pair (a cities file of its own with the two cities and the one pair, role study)
one way. Pairs: the six study pairs of data/ground/cities.json, one way, as listed. Scenarios
(python/make_failures.py): route-satellite, access-satellite, route-laser, plane, region and
gateway (realistic only), chosen from the BASELINE route of the direction at T_i. The baseline
route comes from one `failures` run per (planner, model, instant) with no failure file, all six
directions, over exactly the hand-picked runs' window (so that the whole-interval planner sees
the same interval as theirs), reading its `baseline` rows at T_i; the planner group's `start`
variant has baselines of its own, since its routes differ. Each hand-picked run's own baseline
lane must hold that same route at T_i; a run where it does not fails the script.

  main        every scenario, both models, the reaction modes of REACTIONS; permanent.
  transient   route-satellite and route-laser, lasting 10 s and 60 s.
  detection   --detection-s 0.01, 1 and 40; route-satellite and route-laser.
  compute     --compute-s 0.01; every scenario; flooding only (no runs while REACTIONS lacks
              flooding: the other modes do not use the compute time).
  planner     --replan-on-report on (planner and local-repair only: flooding has no planner);
              and --planner start (every mode of REACTIONS); every scenario.
  repointing  --repoint on with --acquisition-s 2, 12, 30 and 100; route-satellite, plane and
              region.
  gateway     the gateway scenario, realistic; the same settings as main. The next-nearest
              operational gateway (real or assumed, great circle) and the failed gateway's
              source (real or assumed) are in scenarios.csv.

After-recovery groups: one step (--duration-s 0), best case, Hypatia's 100 cities
(data/ground/hypatia/cities.json, --roles all: its 4,950 pairs, one way), --reactions planner,
every failure permanent and dated an hour before the step (old news, spec 4.2). Seeds 0 to 99,
shares 0.1%, 0.3%, 1%, 3%, 10% and 30%.

  random-satellites    a random share of the satellites in planes, at T_0.
  random-lasers        a random share of the planned laser links (pattern three), at T_0.
  attack               the share of satellites that carry the most of the 4,950 baseline routes
                       at T_i (from a one-step run with no failures at T_i), at each T_i.
  recovery-repointing  random-satellites and attack again with --repoint on.
  pattern              random-satellites with --pattern plus-grid and six.

Outputs per run under <out>/<group>/: <name>.failures.csv, <name>.measures.csv,
<name>.facts.csv, <name>.network.csv, <name>.summary.txt, <name>.status (exit status and wall
seconds); hand-picked runs also <name>.routes.csv.gz and <name>.events.csv.gz; after-recovery
runs instead stream the routes CSV and keep <name>.recovery.json (per run: rows, the pairs
delivered in the baseline and the reference lane, the extra delay of the pairs delivered in
both), and their summary and measures CSV gzip-compressed (<name>.summary.txt.gz,
<name>.measures.csv.gz: a block or row per pair, every measure none, as nothing fails in the
run).

Recount and cross-check. For every hand-picked run and reaction mode, from the events CSV (spec
4.7 and 4.10 Details), over [t_f, end of run): black-hole and loop seconds (loop none in
planner and local-repair, which cannot loop), lost seconds (baseline delivered, this lane
not), the time to restore (from t_f to the first delivered instant after the lane was not
delivered; 0 if it never was not; none if it never came back) and the time to settle (from t_f
to the end of the last stretch where the lane's state or route (satellites and gateway) differs
from the reference lane's; 0 if never; none if that stretch reaches the end); from the routes
CSV, the extra delay at the steps at or after t_f where the lane and the baseline are both
delivered (min, median, max) and at the first step at or after the settle instant. Each is held
against the program's measures CSV (times to 1e-6 s, delays to 1e-6 ms), and t_f against the
failure file's instant; the baseline's delivered steps against the summary; the events of every
lane must tile the run. For after-recovery runs the reachability of the baseline and the
reference lanes and the extra delay are counted here from the streamed routes rows; the rows
streamed and the baseline's delivered pairs are held against the summary. Any difference fails
the script, naming the run and the measure.

Summaries under docs/phase-4-data/:

  runs.csv  one row per run: run, group, scenario (the hand-picked scenario, or the
            after-recovery draw: random-satellites, random-lasers, attack), variant, model,
            from, to (hand-picked), instant (i), instant_utc, seed, share (after recovery),
            pattern, planner, replan_on_report, repoint, detection_s, compute_s,
            acquisition_s, length_s (transient), reactions, start_utc, duration_s,
            exit_status, wall_s, facts, facts_old, repair_links_made, repair_links_dropped,
            local_repairs, reports, report_hops, then every check count of the summary as
            <check>_<name>: route_check_* and physical_check_* (Phase 3's, on the baseline),
            failure_check_*, other_lanes_route_check_*, force_check_*.
  scenarios.csv  per hand-picked run, and per skipped one: run, group, scenario, variant,
            model, from, to, instant, instant_utc, status (run or skipped), reason (why it was
            skipped), kind (satellite, laser_link or gateway), satellites, links, gateways
            (how many fail), element (the one element of a single-element scenario, as the
            program writes it), length_s, baseline_route (catalogue numbers), baseline_gateway,
            baseline_ms (the baseline route and delay at T_i), gateway_source, next_gateway,
            next_gateway_source, next_gateway_km (gateway scenario only).
  measures.csv  per hand-picked run and reaction mode: run, group, scenario, variant, model,
            instant, instant_utc, replan_offset_s (seconds T_i falls after a re-plan instant),
            planner_known_first_s (the earliest planner_known_s of the facts CSV: when the
            planner first knows of the failure, from the run's start; never if it never
            does), then the measures CSV row of the affected direction as the
            program wrote it: from, to, role, pattern, model, strategy, reaction, failure_s,
            black_hole_s, loop_s, lost_s, restore_s, settle_s, extra_min_ms, extra_median_ms,
            extra_max_ms, extra_settled_ms, reports, messages (none where it has none).
  recovery.csv  per after-recovery run: run, group, scenario, pattern, share, seed, instant,
            instant_utc, repoint, satellites_failed, links_failed, pairs, delivered_baseline,
            delivered_reference, reach_baseline, reach_reference (shares of pairs delivered),
            extra_pairs (delivered in both), extra_min_ms, extra_median_ms, extra_p95_ms,
            extra_max_ms (reference minus baseline over those pairs), working_satellites,
            largest_piece_share (all shells), largest_piece_share_<shell> per shell (the
            network CSV at the step), repair_links (made, from the summary).
  recovery_summary.csv  per group, scenario, pattern, share and repoint: runs, then for each
            of reach_reference, extra_median_ms and largest_piece_share: <m>_n (runs where it
            exists), <m>_mean, <m>_min, <m>_max, <m>_p2_5, <m>_p97_5, over the seeds (the
            instants for the attack).

Percentiles are numpy's default (linear interpolation). Rows are in the order the runs are
defined: group, then scenario, variant, model, pair, instant (hand-picked); draw, pattern,
share, seed or instant (after recovery). Exit status 1 if any run (or baseline run) does not
exit 0 with "checks: PASSED" in its summary, or a cross-check fails (nothing is written then),
2 on a usage error.

Usage (inside the CI image):

    python3 python/run_phase4.py --failures build/validate/bin/failures

--groups selects groups; --reuse keeps the finished runs already in --out (so with every run
there it rebuilds the summaries without running the program); --jobs runs at the same time.
--instants, --pairs, --seeds, --shares and --hypatia-pairs limit the runs, for trials only;
a trial, or a subset of the groups, needs a --data-dir other than the committed one.
"""

from __future__ import annotations

import argparse
import bisect
import concurrent.futures
import csv
import gzip
import itertools
import json
import re
import shutil
import subprocess
import sys
import time
from collections import Counter
from dataclasses import dataclass, field, replace
from pathlib import Path

import numpy as np

import make_failures as mf
import snapshot_tools as st
from run_phase3 import great_circle_km

REPO = st.REPO_ROOT
SNAPSHOT = REPO / "data/starlink/2026-11-18"
GROUND = REPO / "data/ground/2026-11-19"
CITIES = REPO / "data/ground/cities.json"
HYPATIA_CITIES = REPO / "data/ground/hypatia/cities.json"
DEFAULT_OUT = REPO / "results/phase-4"
DATA_DIR = REPO / "docs/phase-4-data"

PLAN_TIME = "2026-11-18T21:15:00"
STEP_S = 1
REPLAN_S = 15
INSTANTS = 10
FIRST_OFFSET_S = 300
INSTANT_SPACING_S = 697
LEAD_S = 60  # A run starts at the last re-plan instant at least this long before T_i ...
TAIL_S = 300  # ... and ends this long after it.
STRATEGY = "lowest-delay"
MODELS = ("realistic", "best-case")

# The reaction modes of the hand-picked runs. The program does not have flooding yet; add it
# here once it does: ("planner", "local-repair", "flooding").
REACTIONS = ("planner", "local-repair")
ALL_REACTIONS = ("planner", "local-repair", "flooding")
assert set(REACTIONS) <= set(ALL_REACTIONS)

SHARES = (0.001, 0.003, 0.01, 0.03, 0.1, 0.3)
SEEDS = 100
HAND_GROUPS = ("main", "transient", "detection", "compute", "planner", "repointing", "gateway")
RECOVERY_GROUPS = ("random-satellites", "random-lasers", "attack", "recovery-repointing",
                   "pattern")  # fmt: skip
GROUPS = HAND_GROUPS + RECOVERY_GROUPS
TIME_TOLERANCE_S = 1e-6
DELAY_TOLERANCE_MS = 1e-6


def instant_offset_s(i: int) -> int:
    """T_i in seconds from the plan time."""
    return FIRST_OFFSET_S + i * INSTANT_SPACING_S


def utc_at(offset_s: int) -> str:
    return mf.shifted_utc(PLAN_TIME, offset_s)


def window(i: int) -> tuple[int, int]:
    """(start offset from the plan time, duration) of a hand-picked run at T_i."""
    t = instant_offset_s(i)
    start = (t - LEAD_S) // REPLAN_S * REPLAN_S
    return start, t + TAIL_S - start


# ---------------------------------------------------------------------------------------
# What is run.
# ---------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Settings:
    planner: str = "whole-interval"
    replan_on_report: str = "off"
    repoint: str = "off"
    detection_s: float = 0.1
    hop_processing_s: float = 0.001
    compute_s: float = 0.1
    planner_processing_s: float = 0.0
    acquisition_s: float = 30.0
    pattern: str = "three"
    reactions: tuple[str, ...] = REACTIONS

    def arguments(self) -> list[str]:
        return [
            "--planner", self.planner,
            "--replan-on-report", self.replan_on_report,
            "--repoint", self.repoint,
            "--detection-s", repr(self.detection_s),
            "--hop-processing-s", repr(self.hop_processing_s),
            "--compute-s", repr(self.compute_s),
            "--planner-processing-s", repr(self.planner_processing_s),
            "--acquisition-s", repr(self.acquisition_s),
            "--pattern", self.pattern,
            "--reactions", ",".join(self.reactions),
        ]  # fmt: skip


@dataclass(frozen=True)
class HandRun:
    group: str
    scenario: str
    variant: str
    model: str
    pair: tuple[str, str]
    instant: int
    settings: Settings
    length_s: float | None = None

    @property
    def name(self) -> str:
        return f"{self.scenario}.{self.variant}.{self.model}.{slug(self.pair)}.T{self.instant}"

    @property
    def baseline_key(self) -> tuple[str, str, int]:
        return (self.settings.planner, self.model, self.instant)


@dataclass(frozen=True)
class RecoveryRun:
    group: str
    scenario: str  # random-satellites, random-lasers or attack
    pattern: str
    share: float
    seed: int | None  # None for the attack
    instant: int
    repoint: str

    @property
    def name(self) -> str:
        which = f"seed-{self.seed}" if self.seed is not None else f"T{self.instant}"
        tail = ".repoint" if self.repoint == "on" else ""
        return f"{self.scenario}.{self.pattern}.share-{self.share:g}.{which}{tail}"

    @property
    def settings(self) -> Settings:
        return Settings(pattern=self.pattern, repoint=self.repoint, reactions=("planner",))


def slug(pair: tuple[str, str]) -> str:
    return "-".join(re.sub(r"[^a-z0-9]+", "_", c.lower()).strip("_") for c in pair)


def study_pairs() -> list[tuple[str, str]]:
    data = json.loads(CITIES.read_text())
    return [(p["from"], p["to"]) for p in data["pairs"] if p["role"] == "study"]


def hand_runs(groups, pairs, instants) -> list[HandRun]:
    base = Settings()
    flooding_only = tuple(r for r in REACTIONS if r == "flooding")
    planner_modes = tuple(r for r in REACTIONS if r in ("planner", "local-repair"))
    two = ("route-satellite", "route-laser")
    plans: dict[str, list[tuple[str, tuple, Settings, float | None]]] = {
        "main": [("default", mf.SCENARIOS, base, None)],
        "transient": [(f"length-{n}s", two, base, float(n)) for n in (10, 60)],
        "detection": [
            (f"detection-{d:g}s", two, replace(base, detection_s=d), None)
            for d in (0.01, 1.0, 40.0)
        ],
        "compute": [("compute-0.01s", mf.SCENARIOS, replace(base, compute_s=0.01,
                                                             reactions=flooding_only), None)],
        "planner": [
            ("replan-on-report", mf.SCENARIOS,
             replace(base, replan_on_report="on", reactions=planner_modes), None),
            ("planner-start", mf.SCENARIOS, replace(base, planner="start"), None),
        ],
        "repointing": [
            (f"repoint-acquisition-{a}s", ("route-satellite", "plane", "region"),
             replace(base, repoint="on", acquisition_s=float(a)), None)
            for a in (2, 12, 30, 100)
        ],
        "gateway": [("default", ("gateway",), base, None)],
    }  # fmt: skip
    runs = []
    for group in HAND_GROUPS:
        if group not in groups:
            continue
        for variant, scenarios, settings, length_s in plans[group]:
            if not settings.reactions:
                continue
            for scenario in scenarios:
                for model in MODELS:
                    if scenario == "gateway" and model != "realistic":
                        continue
                    for pair in pairs:
                        for i in instants:
                            runs.append(
                                HandRun(group, scenario, variant, model, pair, i, settings,
                                        length_s)
                            )  # fmt: skip
    return runs


def recovery_runs(groups, shares, seeds, instants) -> list[RecoveryRun]:
    runs = []

    def randoms(group, pattern, repoint):
        for share in shares:
            for seed in seeds:
                runs.append(
                    RecoveryRun(group, "random-satellites", pattern, share, seed, 0, repoint)
                )

    def attacks(group, repoint):
        for share in shares:
            for i in instants:
                runs.append(RecoveryRun(group, "attack", "three", share, None, i, repoint))

    for group in RECOVERY_GROUPS:
        if group not in groups:
            continue
        if group == "random-satellites":
            randoms(group, "three", "off")
        elif group == "random-lasers":
            for share in shares:
                for seed in seeds:
                    runs.append(RecoveryRun(group, "random-lasers", "three", share, seed, 0, "off"))
        elif group == "attack":
            attacks(group, "off")
        elif group == "recovery-repointing":
            randoms(group, "three", "on")
            attacks(group, "on")
        else:
            for pattern in ("plus-grid", "six"):
                randoms(group, pattern, "off")
    return runs


# ---------------------------------------------------------------------------------------
# Running the program.
# ---------------------------------------------------------------------------------------
def common_arguments(exe: Path, cities: Path, roles: str, model: str, start: str,
                     duration_s: int) -> list[str]:  # fmt: skip
    serving = ["--serving", "highest-elevation"] if model == "realistic" else []
    return [
        str(exe),
        "--snapshot", str(SNAPSHOT),
        "--ground", str(GROUND),
        "--cities", str(cities),
        "--roles", roles,
        "--directions", "forward",
        "--plan-time", PLAN_TIME,
        "--start", start,
        "--duration-s", str(duration_s),
        "--step-s", str(STEP_S),
        "--model", model,
        "--strategies", STRATEGY,
        "--replan-s", str(REPLAN_S),
        *serving,
    ]  # fmt: skip


def write_status(path: Path, status: int, elapsed: float) -> None:
    path.write_text(json.dumps({"exit": status, "wall_s": round(elapsed, 1)}))


def gzip_file(src: Path) -> None:
    if src.is_file():
        with src.open("rb") as a, gzip.open(src.with_name(src.name + ".gz"), "wb", 5) as b:
            shutil.copyfileobj(a, b)
        src.unlink()


def run_streamed(command: list[str], summary_path: Path, keep) -> tuple[int, float]:
    """Runs with the routes CSV on standard output (`--out -`), handing every row to keep();
    the summary (standard error) goes to summary_path."""
    began = time.monotonic()
    with summary_path.open("w", encoding="utf-8") as summary:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=summary, text=True)
        assert process.stdout is not None
        for r in csv.DictReader(process.stdout):
            keep(r)
        status = process.wait()
    return status, time.monotonic() - began


@dataclass
class Paths:
    """The files of one run."""

    out: Path
    name: str

    def __getattr__(self, what: str) -> Path:
        suffix = {
            "failures": ".failures.csv",
            "measures": ".measures.csv",
            "measures_gz": ".measures.csv.gz",
            "facts": ".facts.csv",
            "network": ".network.csv",
            "summary": ".summary.txt",
            "summary_gz": ".summary.txt.gz",
            "status": ".status",
            "routes": ".routes.csv",
            "routes_gz": ".routes.csv.gz",
            "events": ".events.csv",
            "events_gz": ".events.csv.gz",
            "recovery": ".recovery.json",
            "rows": ".rows.csv",
        }.get(what)
        if suffix is None:
            raise AttributeError(what)
        return self.out / f"{self.name}{suffix}"


def baseline_hand(exe: Path, out: Path, key: tuple[str, str, int], reuse: bool) -> tuple:
    """The no-failure run of one (planner, model, instant) over the hand-picked window, all
    six study directions; keeps its baseline rows at T_i."""
    planner, model, i = key
    p = Paths(out, f"hand.{planner}.{model}.T{i}")
    if reuse and all(f.is_file() for f in (p.rows, p.summary, p.status)):
        return key, json.loads(p.status.read_text())
    start, duration = window(i)
    at = utc_at(instant_offset_s(i))
    command = common_arguments(exe, CITIES, "study", model, utc_at(start), duration)
    command += [*replace(Settings(), planner=planner, reactions=("planner",)).arguments(),
                "--out", "-"]  # fmt: skip
    kept = []
    status, elapsed = run_streamed(
        command,
        p.summary,
        lambda r: kept.append(r) if r["utc"] == at and r["reaction"] == "baseline" else None,
    )
    write_rows(p.rows, kept)
    write_status(p.status, status, elapsed)
    return key, json.loads(p.status.read_text())


def recovery_baseline_name(pairs: int, i: int) -> str:
    return f"recovery.{pairs}-pairs.T{i}"


def baseline_recovery(exe: Path, out: Path, cities: Path, pairs: int, i: int, reuse: bool):
    """The no-failure one-step run at T_i on Hypatia's pairs, best case, pattern three; keeps
    its baseline rows, and the laser plan (--links-out)."""
    p = Paths(out, recovery_baseline_name(pairs, i))
    links = out / f"{p.name}.links.csv"
    if reuse and all(f.is_file() for f in (p.rows, p.summary, p.status, links)):
        return i, json.loads(p.status.read_text())
    command = common_arguments(exe, cities, "all", "best-case", utc_at(instant_offset_s(i)), 0)
    command += [*RecoveryRun("", "", "three", 0, None, i, "off").settings.arguments(),
                "--links-out", str(links), "--out", "-"]  # fmt: skip
    kept = []
    status, elapsed = run_streamed(
        command, p.summary, lambda r: kept.append(r) if r["reaction"] == "baseline" else None
    )
    write_rows(p.rows, kept)
    write_status(p.status, status, elapsed)
    return i, json.loads(p.status.read_text())


def write_rows(path: Path, rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        if rows:
            w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
            w.writeheader()
            w.writerows(rows)


def read_csv(path: Path) -> list[dict]:
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def execute_hand(exe: Path, out: Path, run: HandRun, cities: Path, reuse: bool) -> tuple:
    p = Paths(out, run.name)
    done = (p.measures, p.facts, p.network, p.summary, p.status, p.routes_gz, p.events_gz)
    if reuse and all(f.is_file() for f in done):
        return run, json.loads(p.status.read_text())
    start, duration = window(run.instant)
    command = common_arguments(exe, cities, "study", run.model, utc_at(start), duration)
    command += [
        *run.settings.arguments(),
        "--failures", str(p.failures),
        "--out", str(p.routes),
        "--events-out", str(p.events),
        "--facts-out", str(p.facts),
        "--measures-out", str(p.measures),
        "--network-out", str(p.network),
    ]  # fmt: skip
    began = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    elapsed = time.monotonic() - began
    p.summary.write_text(result.stdout + result.stderr)
    gzip_file(p.routes)
    gzip_file(p.events)
    write_status(p.status, result.returncode, elapsed)
    return run, json.loads(p.status.read_text())


def execute_recovery(exe: Path, out: Path, run: RecoveryRun, cities: Path, reuse: bool):
    p = Paths(out, run.name)
    done = (p.network, p.summary_gz, p.measures_gz, p.facts, p.status, p.recovery)
    if reuse and all(f.is_file() for f in done):
        return run, json.loads(p.status.read_text())
    command = common_arguments(
        exe, cities, "all", "best-case", utc_at(instant_offset_s(run.instant)), 0
    )
    command += [
        *run.settings.arguments(),
        "--failures", str(p.failures),
        "--measures-out", str(p.measures),
        "--facts-out", str(p.facts),
        "--network-out", str(p.network),
        "--out", "-",
    ]  # fmt: skip
    lanes: dict[str, dict[tuple[str, str], float]] = {"baseline": {}, "reference": {}}
    count = Counter()

    def keep(r: dict) -> None:
        count["rows"] += 1
        count[f"rows_{r['reaction']}"] += 1
        if r["reaction"] in lanes and r["state"] == "delivered":
            lanes[r["reaction"]][(r["from"], r["to"])] = float(r["one_way_ms"])

    status, elapsed = run_streamed(command, p.summary, keep)
    gzip_file(p.summary)
    gzip_file(p.measures)
    result = {
        "rows": count["rows"],
        "rows_by_lane": {k.removeprefix("rows_"): v for k, v in count.items() if k != "rows"},
        "delivered": {lane: len(v) for lane, v in lanes.items()},
        "extra_ms": sorted(
            lanes["reference"][k] - lanes["baseline"][k]
            for k in lanes["reference"]
            if k in lanes["baseline"]
        ),
    }
    p.recovery.write_text(json.dumps(result))
    write_status(p.status, status, elapsed)
    return run, json.loads(p.status.read_text())


# ---------------------------------------------------------------------------------------
# Inputs: cities files, planes, gateways.
# ---------------------------------------------------------------------------------------
def write_pair_cities(path: Path, pair: tuple[str, str]) -> None:
    data = json.loads(CITIES.read_text())
    by_name = {c["name"]: c for c in data["cities"]}
    pair_record = next(p for p in data["pairs"] if (p["from"], p["to"]) == pair)
    content = {
        "note": f"One pair of data/ground/cities.json for python/run_phase4.py: {pair[0]} to "
        f"{pair[1]}.",
        "cities": [by_name[pair[0]], by_name[pair[1]]],
        "pairs": [{**pair_record, "role": "study"}],
    }
    write_if_changed(path, json.dumps(content, indent=1) + "\n")


def write_hypatia_cities(path: Path, limit: int | None) -> Path:
    if limit is None:
        return HYPATIA_CITIES
    data = json.loads(HYPATIA_CITIES.read_text())
    pairs = data["pairs"][:limit]
    names = {n for p in pairs for n in (p["from"], p["to"])}
    content = {
        "note": f"The first {limit} pairs of data/ground/hypatia/cities.json (a trial).",
        "cities": [c for c in data["cities"] if c["name"] in names],
        "pairs": pairs,
    }
    write_if_changed(path, json.dumps(content, indent=1) + "\n")
    return path


def write_if_changed(path: Path, text: str) -> None:
    """Writes text, refusing to change a file a kept run already used."""
    if path.is_file():
        if path.read_text(encoding="utf-8") != text:
            raise SystemExit(f"error: {path} exists with other content; use a fresh --out")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def failure_file_text(failures, comments) -> str:
    lines = [f"# {c}" for c in comments] + [mf.HEADER] + [mf.failure_line(f) for f in failures]
    return "\n".join(lines) + "\n"


def run_constellation(exe: Path, out_csv: Path) -> None:
    if out_csv.is_file():
        return
    result = subprocess.run(
        [str(exe), "--snapshot", str(SNAPSHOT), "--time", PLAN_TIME, "--out", str(out_csv)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(f"error: constellation exited with {result.returncode}\n{result.stderr}")


def load_gateways() -> dict[str, tuple[float, float, str]]:
    """Operational gateways: name -> (lat, lon, source real or assumed)."""
    found = {}
    for file, source in (("gateways.json", "real"), ("assumed_gateways.json", "assumed")):
        for g in json.loads((GROUND / file).read_text()):
            if g["status"] == "operational":
                found[g["name"]] = (g["lat"], g["lon"], source)
    return found


def next_gateway(name: str, gateways: dict) -> tuple[str, str, float]:
    lat, lon, _ = gateways[name]
    best = min((great_circle_km(lat, lon, g[0], g[1]), n) for n, g in gateways.items() if n != name)
    return best[1], gateways[best[1]][2], best[0]


# ---------------------------------------------------------------------------------------
# The program's summary.
# ---------------------------------------------------------------------------------------
_CHECK_LINE = re.compile(r"^\s*(.+?) failures(?: of the other lanes)? \(must be 0\): (.*)$", re.M)
_TIMELINE = re.compile(r"facts (\d+) \(old (\d+)\), repair links made (\d+), dropped (\d+)")
_REPORTS = re.compile(r"reports: (\d+), their hops (\d+);.*local repairs (\d+)")
_ROWS = re.compile(r"rows written: (\d+)")
_OK = re.compile(r"^      lowest-delay: ok (\d+) of (\d+) steps", re.M)
_HOME = re.compile(r"^    (.+): (\S+) \(.*\), level|^    (.+): none, level none$", re.M)


def check_counts(text: str) -> dict[str, int]:
    counts = {}
    for m in _CHECK_LINE.finditer(text):
        label = m.group(1).strip().replace("-", "_").replace(" ", "_")
        if "of the other lanes" in m.group(0):
            label = "other_lanes_" + label
        for k, v in re.findall(r"([a-z_]+) (\d+)", m.group(2)):
            counts[f"{label}_{k}"] = int(v)
    return counts


def parse_summary(text: str) -> dict:
    timeline = _TIMELINE.search(text)
    reports = _REPORTS.search(text)
    rows = _ROWS.search(text)
    checks = check_counts(text)
    return {
        "passed": "checks: PASSED" in text,
        "checks": checks,
        "facts": int(timeline.group(1)) if timeline else None,
        "facts_old": int(timeline.group(2)) if timeline else None,
        "repair_links_made": int(timeline.group(3)) if timeline else None,
        "repair_links_dropped": int(timeline.group(4)) if timeline else None,
        "reports": int(reports.group(1)) if reports else None,
        "report_hops": int(reports.group(2)) if reports else None,
        "local_repairs": int(reports.group(3)) if reports else None,
        "rows_written": int(rows.group(1)) if rows else None,
        "ok_steps": [int(m.group(1)) for m in _OK.finditer(text)],
    }


def no_home_pop(text: str) -> set[str]:
    """Cities the summary lists with no home PoP."""
    return {m.group(3) for m in _HOME.finditer(text) if m.group(3)}


def run_failed(status: dict, parsed: dict) -> bool:
    return status["exit"] != 0 or not parsed["passed"] or any(parsed["checks"].values())


# ---------------------------------------------------------------------------------------
# The recount of a hand-picked run.
# ---------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Interval:
    start_s: float
    end_s: float
    state: str
    route: str
    gateway: str


def lanes_of(events: list[dict]) -> dict[tuple, list[Interval]]:
    lanes: dict[tuple, list[Interval]] = {}
    for e in events:
        key = (e["from"], e["to"], e["strategy"], e["reaction"])
        lanes.setdefault(key, []).append(
            Interval(float(e["start_s"]), float(e["end_s"]), e["state"], e["route"], e["gateway"])
        )
    for v in lanes.values():
        v.sort(key=lambda x: x.start_s)
    return lanes


def sample(lane: list[Interval], t: float) -> Interval:
    starts = [x.start_s for x in lane]
    k = bisect.bisect_right(starts, t) - 1
    if k < 0 or not lane[k].start_s <= t < lane[k].end_s:
        raise LookupError(f"no interval at {t}")
    return lane[k]


def recount_times(lane, baseline, reference, t_f: float, end_s: float, reaction: str) -> dict:
    """Spec 4.7 over [t_f, end_s), cut at every interval start of the three lanes."""
    starts = {x.start_s for x in (*lane, *baseline, *reference)}
    cuts = sorted({t_f, end_s} | {t for t in starts if t_f < t < end_s})
    black_hole = loop = lost = 0.0
    interrupted = False
    restored_at = None
    last_differs = None
    for a, b in itertools.pairwise(cuts):
        mine, base, ref = sample(lane, a), sample(baseline, a), sample(reference, a)
        length = b - a
        if mine.state == "black_hole":
            black_hole += length
        elif mine.state == "loop":
            loop += length
        if base.state == "delivered" and mine.state != "delivered":
            lost += length
        if mine.state != "delivered":
            interrupted = True
        elif interrupted and restored_at is None:
            restored_at = a
        if (mine.state, mine.route, mine.gateway) != (ref.state, ref.route, ref.gateway):
            last_differs = b
    restore = 0.0 if not interrupted else (restored_at - t_f if restored_at is not None else None)
    if last_differs is None:
        settle = 0.0
    elif last_differs < end_s:
        settle = last_differs - t_f
    else:
        settle = None
    return {
        "failure_s": t_f,
        "black_hole_s": black_hole,
        "loop_s": loop if reaction == "flooding" else None,
        "lost_s": lost,
        "restore_s": restore,
        "settle_s": settle,
    }


def recount_extras(routes: list[dict], direction, reaction: str, t_f: float, settle) -> dict:
    """Extra delay at the steps at or after t_f where the lane and the baseline are both
    delivered, from the routes CSV; and at the first step at or after the settle instant."""
    delays: dict[str, dict[str, float | None]] = {reaction: {}, "baseline": {}}
    for r in routes:
        if (r["from"], r["to"]) == direction and r["reaction"] in delays:
            ok = r["state"] == "delivered"
            delays[r["reaction"]][r["utc"]] = float(r["one_way_ms"]) if ok else None
    utcs = sorted(delays["baseline"])
    start = st.parse_utc(utcs[0])
    extras, settled = [], None
    settled_seen = False
    for utc in utcs:
        t = st.seconds_between(st.parse_utc(utc), start)
        if t < t_f:
            continue
        mine, base = delays[reaction].get(utc), delays["baseline"][utc]
        both = mine is not None and base is not None
        if both:
            extras.append(mine - base)
        if settle is not None and not settled_seen and t >= t_f + settle:
            settled_seen = True
            settled = mine - base if both else None
    return {
        "extra_min_ms": min(extras) if extras else None,
        "extra_median_ms": float(np.median(extras)) if extras else None,
        "extra_max_ms": max(extras) if extras else None,
        "extra_settled_ms": settled,
    }


def measure_value(text: str) -> float | None:
    return None if text in ("none", "") else float(text)


def cross_check_hand(
    run: HandRun, p: Paths, route: mf.BaselineRoute, parsed: dict
) -> tuple[list[str], list[dict]]:
    """Recounts the measures of every reaction mode of the run; returns the differences and the
    program's measures rows of the affected direction."""
    bad = []

    def differ(what, mine, theirs):
        bad.append(f"{p.name} ({run.group}): {what}: recount {mine}, program {theirs}")

    start, duration = window(run.instant)
    t_f = float(instant_offset_s(run.instant) - start)
    events = read_csv(p.events_gz)
    routes = read_csv(p.routes_gz)
    lanes = lanes_of(events)
    direction = run.pair
    key = (*direction, STRATEGY)
    for lane_key, lane in lanes.items():
        edge = 0.0
        for x in lane:
            if x.start_s != edge or not x.end_s > x.start_s:
                differ(f"intervals of {lane_key[3]} tile the run", f"gap at {edge}", x.start_s)
                break
            edge = x.end_s
        if edge != float(duration):
            differ(f"intervals of {lane_key[3]} end at the run's end", duration, edge)
    baseline, reference = lanes.get((*key, "baseline")), lanes.get((*key, "reference"))
    if baseline is None or reference is None:
        differ("lanes", "baseline and reference", sorted(k[3] for k in lanes))
        return bad, []
    at = sample(baseline, t_f)
    held = (at.state, at.route, at.gateway)
    chosen = ("delivered", " ".join(map(str, route.satellites)), route.gateway)
    if held != chosen:
        differ("baseline route at the failure instant", chosen, held)
    ok_steps = sum(1 for r in routes if r["reaction"] == "baseline" and r["state"] == "delivered")
    if parsed["ok_steps"] != [ok_steps]:
        differ("baseline delivered steps", ok_steps, parsed["ok_steps"])
    rows = [r for r in read_csv(p.measures) if (r["from"], r["to"]) == direction]
    if sorted(r["reaction"] for r in rows) != sorted(run.settings.reactions):
        differ("measures rows", sorted(run.settings.reactions), [r["reaction"] for r in rows])
    for row in rows:
        reaction = row["reaction"]
        lane = lanes.get((*key, reaction))
        if lane is None:
            differ(f"{reaction}: lane", "present", "missing")
            continue
        mine = recount_times(lane, baseline, reference, t_f, float(duration), reaction)
        mine |= recount_extras(routes, direction, reaction, t_f, mine["settle_s"])
        for what, value in mine.items():
            theirs = measure_value(row[what])
            tolerance = DELAY_TOLERANCE_MS if what.endswith("_ms") else TIME_TOLERANCE_S
            if (value is None) != (theirs is None) or (
                value is not None and abs(value - theirs) > tolerance
            ):
                differ(f"{reaction}: {what}", value, row[what])
    return bad, rows


# ---------------------------------------------------------------------------------------
# Rows of the summaries.
# ---------------------------------------------------------------------------------------
def write_csv(path: Path, columns: list[str], rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=columns, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


def fmt(value, decimals: int = 6) -> str:
    return "" if value is None else f"{value:.{decimals}f}"


def settings_values(s: Settings) -> dict:
    return {
        "pattern": s.pattern,
        "planner": s.planner,
        "replan_on_report": s.replan_on_report,
        "repoint": s.repoint,
        "detection_s": f"{s.detection_s:g}",
        "compute_s": f"{s.compute_s:g}",
        "acquisition_s": f"{s.acquisition_s:g}",
        "reactions": " ".join(s.reactions),
    }


def summary_values(status: dict, parsed: dict) -> dict:
    return {
        "exit_status": status["exit"],
        "wall_s": status["wall_s"],
        **{k: parsed[k] for k in ("facts", "facts_old", "repair_links_made",
                                  "repair_links_dropped", "local_repairs", "reports",
                                  "report_hops")},
        **parsed["checks"],
    }  # fmt: skip


def hand_id(run: HandRun) -> dict:
    return {
        "run": run.name,
        "group": run.group,
        "scenario": run.scenario,
        "variant": run.variant,
        "model": run.model,
        "from": run.pair[0],
        "to": run.pair[1],
        "instant": run.instant,
        "instant_utc": utc_at(instant_offset_s(run.instant)),
    }


def hand_run_row(run: HandRun, status: dict, parsed: dict) -> dict:
    start, duration = window(run.instant)
    return {
        **hand_id(run),
        "seed": "",
        "share": "",
        **settings_values(run.settings),
        "length_s": "" if run.length_s is None else f"{run.length_s:g}",
        "start_utc": utc_at(start),
        "duration_s": duration,
        **summary_values(status, parsed),
    }


def recovery_id(run: RecoveryRun) -> dict:
    return {
        "run": run.name,
        "group": run.group,
        "scenario": run.scenario,
        "pattern": run.pattern,
        "share": f"{run.share:g}",
        "seed": "" if run.seed is None else run.seed,
        "instant": run.instant,
        "instant_utc": utc_at(instant_offset_s(run.instant)),
        "repoint": run.repoint,
    }


def recovery_run_row(run: RecoveryRun, status: dict, parsed: dict) -> dict:
    return {
        "run": run.name,
        "group": run.group,
        "scenario": run.scenario,
        "variant": "",
        "model": "best-case",
        "from": "",
        "to": "",
        "instant": run.instant,
        "instant_utc": utc_at(instant_offset_s(run.instant)),
        "seed": "" if run.seed is None else run.seed,
        "share": f"{run.share:g}",
        **settings_values(run.settings),
        "length_s": "",
        "start_utc": utc_at(instant_offset_s(run.instant)),
        "duration_s": 0,
        **summary_values(status, parsed),
    }


SCENARIO_COLUMNS = [
    "run", "group", "scenario", "variant", "model", "from", "to", "instant", "instant_utc",
    "status", "reason", "kind", "satellites", "links", "gateways", "element", "length_s",
    "baseline_route", "baseline_gateway", "baseline_ms", "gateway_source", "next_gateway",
    "next_gateway_source", "next_gateway_km",
]  # fmt: skip
MEASURE_PROGRAM_COLUMNS = [
    "from", "to", "role", "pattern", "model", "strategy", "reaction", "failure_s",
    "black_hole_s", "loop_s", "lost_s", "restore_s", "settle_s", "extra_min_ms",
    "extra_median_ms", "extra_max_ms", "extra_settled_ms", "reports", "messages",
]  # fmt: skip
MEASURE_COLUMNS = [
    "run", "group", "scenario", "variant", "instant", "instant_utc", "replan_offset_s",
    "planner_known_first_s", *MEASURE_PROGRAM_COLUMNS,
]  # fmt: skip


def scenario_row(run: HandRun, scenario: mf.Scenario, route, gateways) -> dict:
    kinds = sorted({f.kind for f in scenario.failures})
    row = {
        **hand_id(run),
        "status": "skipped" if scenario.skipped else "run",
        "reason": scenario.skipped,
        "kind": " ".join(kinds),
        "satellites": scenario.satellites,
        "links": scenario.links,
        "gateways": scenario.gateways,
        "element": scenario.element,
        "length_s": "" if run.length_s is None else f"{run.length_s:g}",
        "baseline_route": " ".join(map(str, route.satellites)) if route else "",
        "baseline_gateway": route.gateway if route else "",
        "baseline_ms": fmt(route.one_way_ms) if route else "",
        "gateway_source": "",
        "next_gateway": "",
        "next_gateway_source": "",
        "next_gateway_km": "",
    }
    if run.scenario == "gateway" and not scenario.skipped:
        name = scenario.failures[0].gateway
        nearest, source, km = next_gateway(name, gateways)
        row |= {
            "gateway_source": gateways[name][2],
            "next_gateway": nearest,
            "next_gateway_source": source,
            "next_gateway_km": fmt(km, 1),
        }
    return row


def network_at_step(path: Path) -> dict[str, dict]:
    rows = read_csv(path)
    if len({r["utc"] for r in rows}) != 1:
        raise SystemExit(f"error: {path} should hold one step")
    return {r["scope"]: r for r in rows}


def percentile(values, q):
    return float(np.percentile(values, q)) if len(values) else None


RECOVERY_METRICS = ("reach_reference", "extra_median_ms", "largest_piece_share")


def recovery_row(run: RecoveryRun, p: Paths, parsed: dict, pairs: int) -> tuple[dict, list]:
    bad = []
    result = json.loads(p.recovery.read_text())
    if result["rows"] != parsed["rows_written"]:
        bad.append(f"{p.name}: rows streamed {result['rows']}, written {parsed['rows_written']}")
    for lane in ("baseline", "reference", "planner"):
        if result["rows_by_lane"].get(lane) != pairs:
            bad.append(f"{p.name}: {lane} rows {result['rows_by_lane'].get(lane)}, pairs {pairs}")
    ok = sum(parsed["ok_steps"])
    if len(parsed["ok_steps"]) != pairs or ok != result["delivered"]["baseline"]:
        bad.append(
            f"{p.name}: baseline delivered pairs: recount {result['delivered']['baseline']}, "
            f"summary {ok} over {len(parsed['ok_steps'])} directions"
        )
    text = p.failures.read_text().splitlines()
    kinds = Counter(line.split(",")[0] for line in text if line and not line.startswith("#"))
    network = network_at_step(p.network)
    extra = result["extra_ms"]
    row = {
        **recovery_id(run),
        "satellites_failed": kinds["satellite"],
        "links_failed": kinds["laser_link"],
        "pairs": pairs,
        "delivered_baseline": result["delivered"]["baseline"],
        "delivered_reference": result["delivered"]["reference"],
        "reach_baseline": result["delivered"]["baseline"] / pairs,
        "reach_reference": result["delivered"]["reference"] / pairs,
        "extra_pairs": len(extra),
        "extra_min_ms": min(extra) if extra else None,
        "extra_median_ms": float(np.median(extra)) if extra else None,
        "extra_p95_ms": percentile(extra, 95),
        "extra_max_ms": max(extra) if extra else None,
        "working_satellites": network["all"]["working_satellites"],
        "largest_piece_share": measure_value(network["all"]["largest_piece_share"]),
        **{
            f"largest_piece_share_{scope}": fmt(measure_value(r["largest_piece_share"]))
            for scope, r in network.items()
            if scope != "all"
        },
        "repair_links": parsed["repair_links_made"],
    }
    return row, bad


def recovery_text(row: dict) -> dict:
    out = dict(row)
    for k in ("reach_baseline", "reach_reference", "largest_piece_share"):
        out[k] = fmt(row[k], 6)
    for k in ("extra_min_ms", "extra_median_ms", "extra_p95_ms", "extra_max_ms"):
        out[k] = fmt(row[k], 6)
    return out


def recovery_summary(rows: list[dict]) -> list[dict]:
    groups: dict[tuple, list[dict]] = {}
    for r in rows:
        groups.setdefault((r["group"], r["scenario"], r["pattern"], r["share"], r["repoint"]),
                          []).append(r)  # fmt: skip
    out = []
    for (group, scenario, pattern, share, repoint), members in groups.items():
        row = {"group": group, "scenario": scenario, "pattern": pattern, "share": share,
               "repoint": repoint, "runs": len(members)}  # fmt: skip
        for m in RECOVERY_METRICS:
            values = [r[m] for r in members if r[m] is not None]
            row[f"{m}_n"] = len(values)
            row[f"{m}_mean"] = fmt(float(np.mean(values)) if values else None)
            row[f"{m}_min"] = fmt(min(values) if values else None)
            row[f"{m}_max"] = fmt(max(values) if values else None)
            row[f"{m}_p2_5"] = fmt(percentile(values, 2.5))
            row[f"{m}_p97_5"] = fmt(percentile(values, 97.5))
        out.append(row)
    return out


def union_columns(rows: list[dict]) -> list[str]:
    columns: list[str] = []
    for row in rows:
        columns += [k for k in row if k not in columns]
    return columns


# ---------------------------------------------------------------------------------------
@dataclass
class Plan:
    """Everything prepared before the runs."""

    hand: list[HandRun] = field(default_factory=list)
    scenarios: dict[tuple[str, str], tuple[mf.Scenario, mf.BaselineRoute | None]] = field(
        default_factory=dict
    )
    recovery: list[RecoveryRun] = field(default_factory=list)


def parallel(jobs: int, calls: list):
    """Runs every (function, *arguments) of calls, at most jobs at a time; yields each result
    as it comes."""
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(f, *a) for f, *a in calls]
        for future in concurrent.futures.as_completed(futures):
            yield future.result()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run the Phase 4 failure runs.")
    parser.add_argument("--failures", type=Path, required=True, help="the failures program")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--data-dir", type=Path, default=DATA_DIR, help="summary directory")
    parser.add_argument("--jobs", type=int, default=4, help="runs at the same time")
    parser.add_argument("--reuse", action="store_true", help="keep finished runs already in --out")
    parser.add_argument("--groups", default=",".join(GROUPS), help="comma-separated groups")
    parser.add_argument("--instants", help="trial: comma-separated instant indices")
    parser.add_argument("--pairs", type=int, help="trial: the first N study pairs")
    parser.add_argument("--seeds", type=int, help="trial: seeds 0 to N - 1")
    parser.add_argument("--shares", help="trial: comma-separated shares")
    parser.add_argument("--hypatia-pairs", type=int, help="trial: the first N Hypatia pairs")
    args = parser.parse_args(argv)

    exe = args.failures
    constellation = exe.parent / "constellation"
    propagate = exe.parent / "propagate"
    for program in (exe, constellation, propagate):
        if not program.is_file():
            print(f"error: {program} not found", file=sys.stderr)
            return 2
    groups = [g for g in args.groups.split(",") if g]
    unknown = [g for g in groups if g not in GROUPS]
    if unknown or not groups:
        print(f"error: unknown groups {unknown}; groups are {', '.join(GROUPS)}", file=sys.stderr)
        return 2
    try:
        instants = [int(x) for x in args.instants.split(",")] if args.instants else None
        shares = [float(x) for x in args.shares.split(",")] if args.shares else None
    except ValueError:
        print("error: --instants and --shares take comma-separated numbers", file=sys.stderr)
        return 2
    trial = any(x is not None for x in (instants, args.pairs, args.seeds, shares,
                                        args.hypatia_pairs))  # fmt: skip
    if (trial or set(groups) != set(GROUPS)) and args.data_dir.resolve() == DATA_DIR.resolve():
        print("error: a trial or a subset of the groups needs a --data-dir of its own",
              file=sys.stderr)  # fmt: skip
        return 2
    instants = instants if instants is not None else list(range(INSTANTS))
    if any(not 0 <= i < INSTANTS for i in instants):
        print(f"error: --instants must be in 0 .. {INSTANTS - 1}", file=sys.stderr)
        return 2
    pairs = study_pairs()[: args.pairs] if args.pairs else study_pairs()
    seeds = list(range(args.seeds if args.seeds is not None else SEEDS))
    shares = shares if shares is not None else list(SHARES)

    out = args.out
    inputs = out / "inputs"
    inputs.mkdir(parents=True, exist_ok=True)
    run_constellation(constellation, inputs / "constellation.csv")
    planes = mf.read_planes(inputs / "constellation.csv")
    gateways = load_gateways()

    # Hand-picked: baselines, then the scenarios.
    plan = Plan(hand=hand_runs(groups, pairs, instants))
    baselines_dir = out / "baselines"
    baselines_dir.mkdir(parents=True, exist_ok=True)
    failed = []
    keys = sorted({r.baseline_key for r in plan.hand})
    for key, status in parallel(args.jobs, [(baseline_hand, exe, baselines_dir, k, args.reuse)
                                            for k in keys]):  # fmt: skip
        print(f"baseline {key}: exit {status['exit']} ({status['wall_s']:.0f} s)", flush=True)
    baseline_routes, missing_pop = {}, {}
    for key in keys:
        p = Paths(baselines_dir, f"hand.{key[0]}.{key[1]}.T{key[2]}")
        text = p.summary.read_text()
        if run_failed(json.loads(p.status.read_text()), parse_summary(text)):
            failed.append(f"{p.name}: the baseline run failed")
            continue
        baseline_routes[key] = mf.baseline_routes(read_csv(p.rows),
                                                  utc_at(instant_offset_s(key[2])))  # fmt: skip
        missing_pop[key] = no_home_pop(text)
    if failed:
        print("FAIL:", *failed, sep="\n  ", file=sys.stderr)
        return 1
    points_cache: dict[int, dict] = {}

    def points_at(i: int):
        def get():
            if i not in points_cache:
                points_cache[i] = mf.subsatellite_points(
                    propagate, SNAPSHOT, utc_at(instant_offset_s(i)), sorted(planes),
                    inputs / f"points.T{i}.csv",
                )  # fmt: skip
            return points_cache[i]

        return get

    to_run = []
    for run in plan.hand:
        group_dir = out / run.group
        cities = inputs / f"pair.{slug(run.pair)}.json"
        write_pair_cities(cities, run.pair)
        route = baseline_routes[run.baseline_key].get(run.pair)
        at = utc_at(instant_offset_s(run.instant))
        if run.model == "realistic" and run.pair[0] in missing_pop[run.baseline_key]:
            scenario = mf.skipped(f"the sender {run.pair[0]} has no home PoP")
        else:
            scenario = mf.hand_picked(run.scenario, route, run.model, at, planes,
                                      points_at(run.instant), run.length_s)  # fmt: skip
        plan.scenarios[(run.group, run.name)] = (scenario, route)
        if scenario.skipped:
            continue
        comments = [
            f"Phase 4 hand-picked scenario {run.scenario} ({run.group}, {run.variant}), "
            f"{run.pair[0]} -> {run.pair[1]}, {run.model}",
            f"failure instant T{run.instant} = {at}; "
            + ("permanent" if run.length_s is None else f"transient, {run.length_s:g} s")
            + f"; from the baseline route {' '.join(map(str, route.satellites))}"
            + (f" via {route.gateway}" if route.gateway else ""),
        ]
        write_if_changed(Paths(group_dir, run.name).failures,
                         failure_file_text(scenario.failures, comments))  # fmt: skip
        to_run.append((execute_hand, exe, group_dir, run, cities, args.reuse))

    # After recovery: baselines for the attack and the laser plan, then the draws.
    plan.recovery = recovery_runs(groups, shares, seeds, instants)
    hypatia = write_hypatia_cities(
        inputs / f"hypatia-first-{args.hypatia_pairs}.json", args.hypatia_pairs
    )
    hypatia_pairs = len(json.loads(hypatia.read_text())["pairs"])
    needed = sorted({r.instant for r in plan.recovery if r.scenario == "attack"}
                    | ({0} if any(r.scenario == "random-lasers" for r in plan.recovery)
                       else set()))  # fmt: skip
    calls = [(baseline_recovery, exe, baselines_dir, hypatia, hypatia_pairs, i, args.reuse)
             for i in needed]  # fmt: skip
    for i, status in parallel(args.jobs, calls):
        print(f"baseline recovery T{i}: exit {status['exit']} ({status['wall_s']:.0f} s)")
    recovery_routes = {}
    for i in needed:
        p = Paths(baselines_dir, recovery_baseline_name(hypatia_pairs, i))
        if run_failed(json.loads(p.status.read_text()), parse_summary(p.summary.read_text())):
            failed.append(f"{p.name}: the baseline run failed")
            continue
        recovery_routes[i] = mf.baseline_routes(read_csv(p.rows), utc_at(instant_offset_s(i)))
        if len(recovery_routes[i]) != hypatia_pairs:
            failed.append(f"{p.name}: {len(recovery_routes[i])} baseline rows, {hypatia_pairs} "
                          "pairs")  # fmt: skip
    if failed:
        print("FAIL:", *failed, sep="\n  ", file=sys.stderr)
        return 1
    links = None
    if 0 in needed:
        plan_name = recovery_baseline_name(hypatia_pairs, 0)
        links = mf.read_links(baselines_dir / f"{plan_name}.links.csv")
    for run in plan.recovery:
        step = utc_at(instant_offset_s(run.instant))
        dated = mf.shifted_utc(step, -mf.OLD_NEWS_S)
        if run.scenario == "random-satellites":
            chosen = mf.random_satellites(planes, run.share, run.seed)
            failures = mf.satellite_failures(chosen, dated)
            what = (f"random satellites: share {run.share:g} of {len(planes)} in planes, "
                    f"{len(chosen)} drawn, seed {run.seed}")  # fmt: skip
        elif run.scenario == "random-lasers":
            chosen = mf.random_links(links, run.share, run.seed)
            failures = mf.link_failures(chosen, dated)
            what = (f"random laser links: share {run.share:g} of {len(links)} planned, "
                    f"{len(chosen)} drawn, seed {run.seed}")  # fmt: skip
        else:
            load = mf.route_load(recovery_routes[run.instant].values())
            chosen = mf.attack_satellites(planes, load, run.share)
            failures = mf.satellite_failures(chosen, dated)
            what = (f"attack: share {run.share:g} of {len(planes)} satellites in planes, the "
                    f"{len(chosen)} most loaded by {hypatia_pairs} baseline routes")  # fmt: skip
        comments = [f"Phase 4 after recovery ({run.group}, pattern {run.pattern}, repoint "
                    f"{run.repoint}): {what}",
                    f"step T{run.instant} = {step}; failures dated {mf.OLD_NEWS_S} s before "
                    "it"]  # fmt: skip
        group_dir = out / run.group
        write_if_changed(Paths(group_dir, run.name).failures,
                         failure_file_text(failures, comments))  # fmt: skip
        to_run.append((execute_recovery, exe, group_dir, run, hypatia, args.reuse))

    print(f"{len(plan.hand)} hand-picked runs defined, {len(plan.recovery)} after recovery; "
          f"running {len(to_run)}", flush=True)  # fmt: skip
    for future_run, status in parallel(args.jobs, to_run):
        print(f"{future_run.group}/{future_run.name}: exit {status['exit']} "
              f"({status['wall_s']:.0f} s)", flush=True)  # fmt: skip

    # Every run must have passed.
    statuses, summaries = {}, {}
    for _, _, group_dir, run, _, _ in to_run:
        p = Paths(group_dir, run.name)
        statuses[(run.group, run.name)] = status = json.loads(p.status.read_text())
        if isinstance(run, RecoveryRun):
            text = gzip.decompress(p.summary_gz.read_bytes()).decode()
        else:
            text = p.summary.read_text()
        summaries[(run.group, run.name)] = parsed = parse_summary(text)
        if run_failed(status, parsed):
            bad_checks = {k: v for k, v in parsed["checks"].items() if v}
            failed.append(f"{run.group}/{run.name}: exit {status['exit']}, passed "
                          f"{parsed['passed']}, failing checks {bad_checks}")  # fmt: skip
    if failed:
        print("FAIL:", *failed, sep="\n  ", file=sys.stderr)
        return 1

    # Recount and cross-check; collect the rows.
    run_rows, scenario_rows, measure_rows, recovery_rows = [], [], [], []
    for run in plan.hand:
        scenario, route = plan.scenarios[(run.group, run.name)]
        scenario_rows.append(scenario_row(run, scenario, route, gateways))
        if scenario.skipped:
            continue
        p = Paths(out / run.group, run.name)
        parsed = summaries[(run.group, run.name)]
        bad, rows = cross_check_hand(run, p, route, parsed)
        failed += bad
        run_rows.append(hand_run_row(run, statuses[(run.group, run.name)], parsed))
        facts = read_csv(p.facts)
        known = [float(f["planner_known_s"]) for f in facts if f["planner_known_s"] != "never"]
        first_known = repr(min(known)) if known else "never"
        offset = instant_offset_s(run.instant) % REPLAN_S
        for row in rows:
            measure_rows.append(
                {
                    "run": run.name,
                    "group": run.group,
                    "scenario": run.scenario,
                    "variant": run.variant,
                    "instant": run.instant,
                    "instant_utc": utc_at(instant_offset_s(run.instant)),
                    "replan_offset_s": offset,
                    "planner_known_first_s": first_known,
                    **{c: row[c] for c in MEASURE_PROGRAM_COLUMNS},
                }
            )
    for run in plan.recovery:
        p = Paths(out / run.group, run.name)
        parsed = summaries[(run.group, run.name)]
        row, bad = recovery_row(run, p, parsed, hypatia_pairs)
        failed += bad
        recovery_rows.append(row)
        run_rows.append(recovery_run_row(run, statuses[(run.group, run.name)], parsed))
    print(f"cross-checked {len(run_rows)} runs: {len(failed)} differences", flush=True)
    if failed:
        print("FAIL:", *failed[:200], sep="\n  ", file=sys.stderr)
        if len(failed) > 200:
            print(f"  ... and {len(failed) - 200} more", file=sys.stderr)
        return 1

    outputs = [
        ("runs.csv", union_columns(run_rows), run_rows),
        ("scenarios.csv", SCENARIO_COLUMNS, scenario_rows),
        ("measures.csv", MEASURE_COLUMNS, measure_rows),
    ]
    if plan.recovery:
        texts = [recovery_text(r) for r in recovery_rows]
        outputs += [
            ("recovery.csv", union_columns(texts), texts),
            ("recovery_summary.csv", union_columns(summary := recovery_summary(recovery_rows)),
             summary),
        ]  # fmt: skip
    for name, columns, rows in outputs:
        write_csv(args.data_dir / name, columns, rows)
        print(f"wrote {args.data_dir / name} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

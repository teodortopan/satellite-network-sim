#!/usr/bin/env python3
"""Run every Phase 2 topology run on the real snapshot and write the committed summaries.

Takes the optimised `topology` program, builds nothing, and runs into an output directory
that is not committed (default results/phase-2/, ignored by git):

  main          2026-11-18T21:15:00 for 12 h every 15 s, all four shells, all pairs (study
                and validation), each pattern (three, plus-grid, six) x model (realistic,
                best-case).
  dev-shell     the same on the development shell alone (--shells 53deg).
  sensitivity   pattern three, one parameter changed at a time from the defaults: range cap
                1,000 / 2,000 / 3,000 km, Sun exclusion 5 / 10 deg, lock-on 15 / 60 s (both
                models); fibre factor 1.5 / 2.5 (realistic only); the 5 deg high-latitude mask
                (both models). The main run is the default of each.
  step-60       the main settings every 60 s for 12 h.
  step-1        the main settings every 1 s for the first hour.
  single-shell  pattern three, both models, on the 43deg, 97deg and 70deg shells alone (the
                53deg shell alone is dev-shell): which shell carries which path.

Each run writes <name>.csv.gz (the program's CSV, compressed) and <name>.summary.txt. Then
this script writes the small summaries under docs/phase-2-data/:

  runs.csv          one row per run: settings, exit status, every physical-check count,
                    delays below the lower bound, laser link-steps up and down by reason.
  main.csv, dev_shell.csv, sensitivity.csv, step_runs.csv, single_shell.csv
                    per run, pair and direction: steps by status, one-way delay and stretch
                    (min, median, 95th percentile, max), laser hops (median, max), and the
                    gateways and home PoPs used with their step counts (realistic).
  timestep.csv      per pattern, model and direction: what the time step changes (changes of
                    serving satellite, gateway and path; how long a path holds; the largest
                    jump in delay between consecutive steps; the delay distribution) at 1 s and
                    15 s over the first hour, and at 15 s and 60 s over 12 h.
  plausibility.csv  the realistic model against published Starlink measurements: Oulu to New
                    York round trip, and dish to home PoP round trip per study city.
  flagged.csv       every main-run direction whose median stretch exceeds 3.

Percentiles are numpy's default (linear interpolation). Exit status 1 if any run fails or
reports a physical-check failure or a delay below its lower bound, 2 on a usage error.

Usage (inside the CI image; about 20 minutes with 4 parallel runs):

    python3 python/run_phase2.py --topology build/validate/bin/topology
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
from dataclasses import dataclass
from pathlib import Path

import numpy as np

import snapshot_tools as st

REPO = st.REPO_ROOT
SNAPSHOT = REPO / "data/starlink/2026-11-18"
GROUND = REPO / "data/ground/2026-11-19"
CITIES = REPO / "data/ground/cities.json"
DEFAULT_OUT = REPO / "results/phase-2"
DATA_DIR = REPO / "docs/phase-2-data"

START = "2026-11-18T21:15:00"
DURATION_S = 12 * 3600
STEP_S = 15
PATTERNS = ("three", "plus-grid", "six")
MODELS = ("realistic", "best-case")
C_KM_S = 299_792.458
FIBRE_FACTOR = 1.98
MEAN_RADIUS_KM = (2 * 6378.137 + 6356.752314245179) / 3  # As the program's great circle.

# Published values this model is set against (spec 6.3; reported, never pass or fail).
OULU_NEW_YORK_MEASURED_RTT_MS = 143.0  # Starlink, median (arXiv 2502.15552).
OULU_NEW_YORK_TERRESTRIAL_RTT_MS = 103.0  # Terrestrial, median (same paper).
NEAREST_POP_MEASURED_RTT_MS = 40.0  # About; median to the nearest PoP (arXiv 2310.09242).


@dataclass(frozen=True)
class Run:
    name: str
    group: str
    pattern: str
    model: str
    duration_s: int = DURATION_S
    step_s: int = STEP_S
    extra: tuple[str, ...] = ()
    parameter: str = "default"  # For sensitivity runs: "name=value".


def all_runs() -> list[Run]:
    runs = []
    for pattern in PATTERNS:
        for model in MODELS:
            runs.append(Run(f"main-{pattern}-{model}", "main", pattern, model))
            runs.append(
                Run(
                    f"dev-{pattern}-{model}",
                    "dev-shell",
                    pattern,
                    model,
                    extra=("--shells", "53deg"),
                )
            )
            runs.append(Run(f"step60-{pattern}-{model}", "step-60", pattern, model, step_s=60))
            runs.append(Run(f"step1-{pattern}-{model}", "step-1", pattern, model, 3600, 1))
    sensitivity = (
        [("range-cap-km", v, MODELS) for v in ("1000", "2000", "3000")]
        + [("sun-exclusion-deg", v, MODELS) for v in ("5", "10")]
        + [("lock-on-s", v, MODELS) for v in ("15", "60")]
        + [("fibre-factor", v, ("realistic",)) for v in ("1.5", "2.5")]
        + [("high-latitude-mask", "on", MODELS)]
    )
    for option, value, models in sensitivity:
        for model in models:
            runs.append(
                Run(
                    f"sens-{option}-{value}-{model}",
                    "sensitivity",
                    "three",
                    model,
                    extra=(f"--{option}", value),
                    parameter=f"{option}={value}",
                )
            )
    # Each other shell alone (53deg alone is dev-shell), pattern three: which shell carries
    # which path, for the explanation of the long paths.
    for shell in ("43deg", "97deg", "70deg"):
        for model in MODELS:
            runs.append(
                Run(
                    f"shell-{shell}-three-{model}",
                    "single-shell",
                    "three",
                    model,
                    extra=("--shells", shell),
                    parameter=f"shells={shell}",
                )
            )
    return runs


# ---------------------------------------------------------------------------------------
# Running.
# ---------------------------------------------------------------------------------------
def command(run: Run, topology: Path, csv_path: Path) -> list[str]:
    return [
        str(topology),
        "--snapshot",
        str(SNAPSHOT),
        "--ground",
        str(GROUND),
        "--cities",
        str(CITIES),
        "--start",
        START,
        "--duration-s",
        str(run.duration_s),
        "--step-s",
        str(run.step_s),
        "--pattern",
        run.pattern,
        "--model",
        run.model,
        "--roles",
        "all",
        *run.extra,
        "--out",
        str(csv_path),
    ]


def execute(run: Run, topology: Path, out: Path, reuse: bool) -> tuple[Run, int, float]:
    gz_path = out / f"{run.name}.csv.gz"
    summary_path = out / f"{run.name}.summary.txt"
    status_path = out / f"{run.name}.status"
    if reuse and gz_path.is_file() and summary_path.is_file() and status_path.is_file():
        return run, int(status_path.read_text()), 0.0
    csv_path = out / f"{run.name}.csv"
    began = time.monotonic()
    result = subprocess.run(
        command(run, topology, csv_path), capture_output=True, text=True, check=False
    )
    elapsed = time.monotonic() - began
    summary_path.write_text(result.stdout + result.stderr)
    if csv_path.is_file():
        with csv_path.open("rb") as src, gzip.open(gz_path, "wb", compresslevel=5) as dst:
            shutil.copyfileobj(src, dst)
        csv_path.unlink()
    status_path.write_text(str(result.returncode))
    return run, result.returncode, elapsed


# ---------------------------------------------------------------------------------------
# Reading the outputs.
# ---------------------------------------------------------------------------------------
def read_rows(out: Path, run: Run) -> list[dict]:
    with gzip.open(out / f"{run.name}.csv.gz", "rt", newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


_CHECKS = re.compile(r"physical-check failures \(must be 0\): (.*)$", re.M)
_LOWER = re.compile(r"delays below the lower bound \(must be 0\): (\d+)")
_LINKS = re.compile(r"link-steps up: (\d+), down: (.*)$", re.M)
_PLANNED = re.compile(r"planned laser links: (\d+)")
_UNUSED = re.compile(r"lasers without a link: (.*)$", re.M)


def pairs_of(text: str) -> dict[str, int]:
    """'a 1, b 2' or 'a 1 b 2' -> {'a': 1, 'b': 2}."""
    return {k: int(v) for k, v in re.findall(r"([a-z_]+) (\d+)", text)}


def parse_summary(text: str) -> dict:
    checks = _CHECKS.search(text)
    lower = _LOWER.search(text)
    links = _LINKS.search(text)
    planned = _PLANNED.search(text)
    unused = _UNUSED.search(text)
    parsed = {
        "checks": pairs_of(checks.group(1)) if checks else None,
        "lower_bound_failures": int(lower.group(1)) if lower else None,
        "link_steps_up": int(links.group(1)) if links else None,
        "link_steps_down": pairs_of(links.group(2)) if links else {},
        "planned_links": int(planned.group(1)) if planned else None,
        "lasers_without_link": pairs_of(unused.group(1)) if unused else {},
        "passed": "checks: PASSED" in text,
    }
    return parsed


def quantiles(values: list[float]) -> list[str]:
    if not values:
        return ["", "", "", ""]
    a = np.asarray(values)
    return [
        f"{a.min():.3f}",
        f"{np.median(a):.3f}",
        f"{np.percentile(a, 95):.3f}",
        f"{a.max():.3f}",
    ]


def counts_text(counter: Counter) -> str:
    return "; ".join(f"{k}: {v}" for k, v in sorted(counter.items(), key=lambda x: (-x[1], x[0])))


DIRECTION_COLUMNS = [
    "run",
    "group",
    "pattern",
    "model",
    "parameter",
    "from",
    "to",
    "role",
    "steps",
    "ok",
    "no_service",
    "no_coverage",
    "no_gateway",
    "no_path",
    "one_way_min_ms",
    "one_way_median_ms",
    "one_way_p95_ms",
    "one_way_max_ms",
    "stretch_min",
    "stretch_median",
    "stretch_p95",
    "stretch_max",
    "laser_hops_median",
    "laser_hops_max",
    "great_circle_km",
    "gateways",
    "home_pops",
]


def direction_rows(run: Run, rows: list[dict]) -> list[dict]:
    by_direction: dict[tuple[str, str], list[dict]] = {}
    for r in rows:
        by_direction.setdefault((r["from"], r["to"]), []).append(r)
    out = []
    for (a, b), rs in by_direction.items():
        status = Counter(r["status"] for r in rs)
        ok = [r for r in rs if r["status"] == "ok"]
        hops = [int(r["laser_hops"]) for r in ok]
        gateways = Counter(f"{r['gateway']} ({r['gateway_source']})" for r in ok if r["gateway"])
        pops = Counter(r["home_pop"] for r in rs if r.get("home_pop"))
        out.append(
            dict(
                zip(
                    DIRECTION_COLUMNS,
                    [
                        run.name,
                        run.group,
                        run.pattern,
                        run.model,
                        run.parameter,
                        a,
                        b,
                        rs[0]["role"],
                        len(rs),
                        status["ok"],
                        status["no_service"],
                        status["no_coverage"],
                        status["no_gateway"],
                        status["no_path"],
                        *quantiles([float(r["one_way_ms"]) for r in ok]),
                        *quantiles([float(r["stretch"]) for r in ok]),
                        f"{np.median(hops):.1f}" if hops else "",
                        max(hops) if hops else "",
                        f"{float(rs[0]['great_circle_km']):.1f}",
                        counts_text(gateways),
                        counts_text(pops),
                    ],
                    strict=True,
                )
            )
        )
    return out


def write_csv(path: Path, columns: list[str], rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=columns, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


# ---------------------------------------------------------------------------------------
# Time step.
# ---------------------------------------------------------------------------------------
TIMESTEP_COLUMNS = [
    "pattern",
    "model",
    "from",
    "to",
    "series",
    "step_s",
    "steps",
    "ok",
    "serving_changes",
    "exit_changes",
    "path_changes",
    "path_hold_median_s",
    "path_hold_max_s",
    "largest_jump_ms",
    "one_way_min_ms",
    "one_way_median_ms",
    "one_way_p95_ms",
    "one_way_max_ms",
]


def series_metrics(rs: list[dict], step_s: float, model: str) -> dict:
    """What a sequence of steps of one direction shows. Serving satellite: realistic
    serving_norad_cat_id, best case first_norad_cat_id. Exit: realistic the gateway, best case
    last_norad_cat_id. Path: (serving, exit satellite, gateway, laser hops)."""
    serving_key = "serving_norad_cat_id" if model == "realistic" else "first_norad_cat_id"
    exit_key = "gateway" if model == "realistic" else "last_norad_cat_id"

    def path(r):
        if r["status"] != "ok":
            return ("none", r["status"])
        last = r["gateway_norad_cat_id"] if model == "realistic" else r["last_norad_cat_id"]
        return (r[serving_key], last, r["gateway"], r["laser_hops"])

    def changes(values):
        return sum(1 for x, y in itertools.pairwise(values) if x != y)

    paths = [path(r) for r in rs]
    holds, run_length = [], 1
    for x, y in itertools.pairwise(paths):
        if x == y:
            run_length += 1
        else:
            holds.append(run_length * step_s)
            run_length = 1
    holds.append(run_length * step_s)
    jumps = [
        abs(float(y["one_way_ms"]) - float(x["one_way_ms"]))
        for x, y in itertools.pairwise(rs)
        if x["status"] == "ok" and y["status"] == "ok"
    ]
    delays = [float(r["one_way_ms"]) for r in rs if r["status"] == "ok"]
    q = quantiles(delays)
    return {
        "steps": len(rs),
        "ok": len(delays),
        "serving_changes": changes([r[serving_key] for r in rs]),
        "exit_changes": changes([r[exit_key] for r in rs]),
        "path_changes": changes(paths),
        "path_hold_median_s": f"{np.median(holds):.0f}",
        "path_hold_max_s": f"{max(holds):.0f}",
        "largest_jump_ms": f"{max(jumps):.3f}" if jumps else "",
        "one_way_min_ms": q[0],
        "one_way_median_ms": q[1],
        "one_way_p95_ms": q[2],
        "one_way_max_ms": q[3],
    }


def timestep_rows(out: Path, runs: dict[str, Run]) -> list[dict]:
    rows = []
    for pattern in PATTERNS:
        for model in MODELS:
            base = read_rows(out, runs[f"main-{pattern}-{model}"])
            fine = read_rows(out, runs[f"step1-{pattern}-{model}"])
            coarse = read_rows(out, runs[f"step60-{pattern}-{model}"])
            start = st.parse_utc(START).unix_ns

            def offset_s(r, start=start):
                return (st.parse_utc(r["utc"]).unix_ns - start) // st.NS_PER_S

            groups: dict[tuple[str, str], dict[str, list[dict]]] = {}
            for name, rs in (("1s-first-hour", fine), ("60s-12h", coarse)):
                for r in rs:
                    groups.setdefault((r["from"], r["to"]), {}).setdefault(name, []).append(r)
            for r in base:
                g = groups.setdefault((r["from"], r["to"]), {})
                t = offset_s(r)
                g.setdefault("15s-12h", []).append(r)
                if t <= 3600:
                    g.setdefault("15s-first-hour", []).append(r)
                if t % 60 == 0:
                    g.setdefault("15s-12h-at-60s-instants", []).append(r)
            steps = {
                "1s-first-hour": 1,
                "15s-first-hour": 15,
                "15s-12h": 15,
                "60s-12h": 60,
                "15s-12h-at-60s-instants": 60,
            }
            for (a, b), series in groups.items():
                for name, step in steps.items():
                    metrics = series_metrics(series[name], step, model)
                    rows.append(
                        {
                            "pattern": pattern,
                            "model": model,
                            "from": a,
                            "to": b,
                            "series": name,
                            "step_s": step,
                            **metrics,
                        }
                    )
            # The 60 s run must be the 15 s run at every fourth instant.
            check = [(r["utc"], r["from"], r["to"], r["one_way_ms"]) for r in coarse]
            again = [
                (r["utc"], r["from"], r["to"], r["one_way_ms"])
                for r in base
                if offset_s(r) % 60 == 0
            ]
            if check != again:
                raise SystemExit(
                    f"error: 60 s and 15 s runs differ at shared instants ({pattern}, {model})"
                )
    return rows


# ---------------------------------------------------------------------------------------
# Plausibility.
# ---------------------------------------------------------------------------------------
def great_circle_km(lat1, lon1, lat2, lon2) -> float:
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    y = math.hypot(
        math.cos(p2) * math.sin(dl),
        math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl),
    )
    x = math.sin(p1) * math.sin(p2) + math.cos(p1) * math.cos(p2) * math.cos(dl)
    return MEAN_RADIUS_KM * math.atan2(y, x)


def load_sites() -> tuple[dict[tuple[str, str], tuple[float, float]], dict[str, tuple]]:
    gateways = {}
    for source, name in (("real", "gateways.json"), ("assumed", "assumed_gateways.json")):
        for g in json.loads((GROUND / name).read_text()):
            gateways.setdefault((g["name"], source), (g["lat"], g["lon"]))
    pops = {
        p["code"]: (p["lat"], p["lon"]) for p in json.loads((GROUND / "pop_codes.json").read_text())
    }
    return gateways, pops


PLAUSIBILITY_COLUMNS = [
    "measure",
    "pattern",
    "from",
    "to",
    "home_pop",
    "steps",
    "model_min_ms",
    "model_median_ms",
    "model_p95_ms",
    "model_max_ms",
    "published_ms",
    "note",
]


def plausibility_rows(out: Path, runs: dict[str, Run]) -> list[dict]:
    gateways, pops = load_sites()
    rows = []
    for pattern in PATTERNS:
        rs = read_rows(out, runs[f"main-{pattern}-realistic"])
        # Oulu to New York, round trip as the program gives it (this direction plus the
        # reverse at the same step).
        rtt = [
            float(r["round_trip_ms"])
            for r in rs
            if r["from"] == "Oulu" and r["to"] == "New York" and r["round_trip_ms"]
        ]
        q = quantiles(rtt)
        rows.append(
            dict(
                zip(
                    PLAUSIBILITY_COLUMNS,
                    [
                        "round trip Oulu - New York",
                        pattern,
                        "Oulu",
                        "New York",
                        "",
                        len(rtt),
                        *q,
                        OULU_NEW_YORK_MEASURED_RTT_MS,
                        f"terrestrial median {OULU_NEW_YORK_TERRESTRIAL_RTT_MS:.0f} ms",
                    ],
                    strict=True,
                )
            )
        )
        # Dish to gateway and back (twice uplink + lasers + downlink, over c), and dish to home
        # PoP and back (plus twice the fibre from the gateway to the home PoP). The path to
        # the home PoP does not depend on the destination, so each sender city counts once
        # per step.
        to_gateway: dict[str, dict[str, float]] = {}
        to_pop: dict[str, dict[str, float]] = {}
        pop_of: dict[str, str] = {}
        for r in rs:
            if r["status"] != "ok":
                continue
            space_km = float(r["uplink_km"]) + float(r["laser_km"]) + float(r["downlink_km"])
            gw = gateways[(r["gateway"], r["gateway_source"])]
            pop = pops[r["home_pop"]]
            fibre_km = great_circle_km(gw[0], gw[1], pop[0], pop[1])
            to_gateway.setdefault(r["from"], {})[r["utc"]] = 2e3 * space_km / C_KM_S
            to_pop.setdefault(r["from"], {})[r["utc"]] = (
                2e3 * (space_km + fibre_km * FIBRE_FACTOR) / C_KM_S
            )
            pop_of[r["from"]] = r["home_pop"]
        for measure, values in (
            ("round trip dish - gateway (space only)", to_gateway),
            ("round trip dish - home PoP", to_pop),
        ):
            for city in sorted(values):
                series = list(values[city].values())
                rows.append(
                    dict(
                        zip(
                            PLAUSIBILITY_COLUMNS,
                            [
                                measure,
                                pattern,
                                city,
                                "",
                                pop_of[city],
                                len(series),
                                *quantiles(series),
                                NEAREST_POP_MEASURED_RTT_MS,
                                "measured: bent pipe about 40 ms median (terminal to the "
                                "ground station's first hop), the PoP about 5 ms beyond",
                            ],
                            strict=True,
                        )
                    )
                )
    return rows


# ---------------------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run the Phase 2 topology runs.")
    parser.add_argument("--topology", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--data", type=Path, default=DATA_DIR, help="summary directory")
    parser.add_argument("--jobs", type=int, default=4, help="runs at the same time")
    parser.add_argument("--reuse", action="store_true", help="keep finished runs already in --out")
    parser.add_argument("--only", help="run only runs whose name contains this text")
    args = parser.parse_args(argv)
    if not args.topology.is_file():
        print(f"error: {args.topology} not found", file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)

    runs = all_runs()
    if args.only:
        runs = [r for r in runs if args.only in r.name]
    by_name = {r.name: r for r in runs}
    failures = []
    run_rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(execute, r, args.topology, args.out, args.reuse) for r in runs]
        for future in concurrent.futures.as_completed(futures):
            run, status, elapsed = future.result()
            print(f"{run.name}: exit {status} ({elapsed:.0f} s)", flush=True)
    for run in runs:
        status = int((args.out / f"{run.name}.status").read_text())
        parsed = parse_summary((args.out / f"{run.name}.summary.txt").read_text())
        check_total = sum(parsed["checks"].values()) if parsed["checks"] else None
        if status != 0 or not parsed["passed"] or check_total or parsed["lower_bound_failures"]:
            failures.append(
                f"{run.name}: exit {status}, checks {parsed['checks']}, "
                f"lower bound {parsed['lower_bound_failures']}"
            )
        run_rows.append(
            {
                "run": run.name,
                "group": run.group,
                "pattern": run.pattern,
                "model": run.model,
                "parameter": run.parameter,
                "duration_s": run.duration_s,
                "step_s": run.step_s,
                "options": " ".join(run.extra),
                "exit_status": status,
                "physical_check_failures": check_total,
                "lower_bound_failures": parsed["lower_bound_failures"],
                "planned_links": parsed["planned_links"],
                "link_steps_up": parsed["link_steps_up"],
                **{f"down_{k}": v for k, v in parsed["link_steps_down"].items()},
                **{f"unused_{k}": v for k, v in parsed["lasers_without_link"].items()},
            }
        )
    if failures:
        print("FAIL:", *failures, sep="\n  ", file=sys.stderr)
        return 1
    if args.only:
        return 0

    columns = list(run_rows[0])
    for row in run_rows:
        columns += [k for k in row if k not in columns]
    write_csv(args.data / "runs.csv", columns, run_rows)
    groups = {
        "main.csv": ("main",),
        "dev_shell.csv": ("dev-shell",),
        "sensitivity.csv": ("sensitivity",),
        "step_runs.csv": ("step-60", "step-1"),
        "single_shell.csv": ("single-shell",),
    }
    for file, group in groups.items():
        rows = []
        for run in runs:
            if run.group in group:
                rows += direction_rows(run, read_rows(args.out, run))
        write_csv(args.data / file, DIRECTION_COLUMNS, rows)
        print(f"wrote {args.data / file} ({len(rows)} rows)")
    timestep = timestep_rows(args.out, by_name)
    write_csv(args.data / "timestep.csv", TIMESTEP_COLUMNS, timestep)
    plausibility = plausibility_rows(args.out, by_name)
    write_csv(args.data / "plausibility.csv", PLAUSIBILITY_COLUMNS, plausibility)
    main_rows = [
        r
        for run in runs
        if run.group == "main"
        for r in direction_rows(run, read_rows(args.out, run))
    ]
    flagged = [r for r in main_rows if r["stretch_median"] and float(r["stretch_median"]) > 3.0]
    write_csv(args.data / "flagged.csv", DIRECTION_COLUMNS, flagged)
    print(
        f"wrote timestep.csv ({len(timestep)}), plausibility.csv ({len(plausibility)}), "
        f"flagged.csv ({len(flagged)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

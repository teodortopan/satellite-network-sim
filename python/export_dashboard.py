#!/usr/bin/env python3
"""Run the engine's programs on the dashboard's fixed scenarios and write the exported data.

docs/phase-5-dashboard.md sections 4.2 and 5. Takes the optimised programs (`propagate`,
`constellation`, `topology`, `routing`, `failures`, all in --bin), builds nothing, keeps every
CSV and summary the programs write under <results>/<small|full>/ (default
results/dashboard/, ignored by git) for python/check_dashboard_data.py, and writes the files
the page loads into --out (default dashboard/public/data/, ignored by git). The exact shape of
every exported file is in dashboard/data-format.md.

Fixed in every run: snapshot data/starlink/2026-11-18, ground data/ground/2026-11-19, and
the shells, planes and laser plans of the plan time of Phases 3 and 4 (2026-11-18T21:15:00).

  Window         full: two hours from the plan time; small (--small): seven minutes from the
                 failure story's run start (21:30:30), so that every story has positions.
                 Sample step 60 s.
  Positions      `propagate` over the window at the sample step, and over the first ten
                 minutes (small: the seven minutes) at 1 s for the error measurement; only the
                 satellites in planes (the `constellation` program at the plan time).
  Laser links    `topology --links-out`, one step at the plan time, best case, study pairs,
                 for plus-grid, three and six.
  Route story    `routing` with Phase 3's main settings (python/run_phase3.py, group main):
                 1 s steps, re-plan 15 s, planner start, pattern three, --backup on, the
                 drops CSV; the best-case model, because in the realistic model five of the
                 six study pairs have no laser hop and every strategy gives the same route
                 (docs/phase-3-results.md). Four strategies: lowest-delay, keep-until-broken,
                 longest-lived and threshold:2ms (the threshold in milliseconds the Phase 3
                 report's table shows halfway between the two extremes). Full: the six study
                 pairs, both directions, the whole window; small: New York to London, both
                 directions, the seven minutes (--plan-time the plan time).
  Failure story  `failures` with Phase 4's default settings (python/run_phase4.py, Settings()
                 and common_arguments), realistic, pattern three, the three reaction modes, at
                 instant T1 (21:31:37), over the hand-picked run's window (21:30:30 for
                 367 s), with the floods CSV and the laser plan written too. The scenarios
                 route-satellite, region and gateway (small: route-satellite), chosen by
                 python/make_failures.py from the baseline route at T1 of a no-failure run
                 made as run_phase4.baseline_hand makes it.
                 The study pair is Reykjavik to Vladivostok: in Phase 4's main T1
                 route-satellite realistic run (docs/phase-4-data/measures.csv) it is the
                 only pair whose three modes clearly differ in kind and not only in time:
                 planner a black hole of 8 s until the next plan, local repair 0.1 s, and
                 flooding a black hole of 0.4 s with a 7 ms loop and a settle time of
                 143.2 s against 8 s; on the five other pairs the three modes differ only in
                 the length of one short black hole.
                 Slow motion: 0.02x for the first second after the failure, then at most 2x
                 until the last lane has settled, after a pause of 2 s (spec 4.5).
  Falling apart  `failures`, one step (--duration-s 0) at T0 (21:20:00; small: T1, since T0
                 is before the small window), Phase 4's
                 after-recovery settings (best case, reaction planner, pattern three, failures
                 dated an hour before the step), on the six study pairs one way. Random
                 satellites at 1%, 10% and 30% with seed 0, and the attack at 30% (the
                 satellites most loaded by the 4,950 baseline routes of Hypatia's 100 cities
                 at that instant, from a one-step run made as run_phase4.baseline_recovery
                 makes it).
                 Small: random satellites at 1%, seed 0.

Values the export works out itself (each is named in dashboard/data-format.md and recomputed
by the data check): times from UTC text as seconds from the window or run start; per route
interval the steps it spans; per satellite and site the first instant it learned any fact of
the failure; the end of the slow-motion spans; per route strategy the number of route
changes (bad plus increases) and the total time without a route; the laser links taken down
in a one-step run (its facts CSV), and so the planned links that did not fail. Everything
else is copied from the programs' CSVs and summaries or from the committed files in
docs/phase-4-data/ and data/ground/.

Exit status 1 if any program exits with an error or its summary reports a failed check
(nothing is exported then), 2 on a usage error.

Usage (inside the CI image):

    python3 python/export_dashboard.py [--small] [--bin build/validate/bin] \\
        [--out dashboard/public/data] [--results results/dashboard] [--jobs 4]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

import make_failures as mf
import run_phase3 as r3
import run_phase4 as r4
import snapshot_tools as st

REPO = st.REPO_ROOT
SNAPSHOT = r4.SNAPSHOT
GROUND = r4.GROUND
CITIES = r4.CITIES
DEFAULT_BIN = REPO / "build/validate/bin"
DEFAULT_OUT = REPO / "dashboard/public/data"
DEFAULT_RESULTS = REPO / "results/dashboard"
PHASE4_DATA = REPO / "docs/phase-4-data"

FORMAT_VERSION = 1
PLAN_TIME = r4.PLAN_TIME
SAMPLE_STEP_S = 60
WINDOW_S = {"full": 2 * 3600, "small": 7 * 60}
ERROR_WINDOW_S = {"full": 10 * 60, "small": 7 * 60}
ERROR_STEP_S = 1
MAX_FILE_BYTES = 25 * 1024 * 1024  # Cloudflare Pages' limit per file (spec 4.2).
PATTERNS = ("plus-grid", "three", "six")

ROUTE_MODEL = "best-case"
ROUTE_STRATEGIES = ("lowest-delay", "keep-until-broken", "longest-lived", "threshold:2ms")
ROUTE_SMALL_PAIR = ("New York", "London")

FAILURE_PAIR = ("Reykjavik", "Vladivostok")
FAILURE_INSTANT = 1
FAILURE_MODEL = "realistic"
FAILURE_SCENARIOS = {
    "full": ("route-satellite", "region", "gateway"),
    "small": ("route-satellite",),
}
LANES = ("baseline", "reference", *r4.ALL_REACTIONS)
# The clock's speed in slow motion (simulated seconds per real second) and the real-time
# pause before the failure. Settings of the export, not results.
# Spans the clock plays slower than chosen: the first second after the failure (the flood and
# the first repairs, milliseconds apart), then at most 2x until the last lane has settled.
FIRST_SPAN_S = 1
FIRST_SPAN_SPEED = 0.02
SETTLE_SPAN_SPEED = 2
PAUSE_BEFORE_FAILURE_REAL_S = 2.0

# Phase 4's instant of the one-step runs: T0, as Phase 4's random draws; in the small export
# T1, since T0 lies before its window.
RESILIENCE_INSTANTS = {"full": 0, "small": 1}
RESILIENCE_STEPS = {
    "full": (
        ("random-satellites", 0.01, 0),
        ("random-satellites", 0.1, 0),
        ("random-satellites", 0.3, 0),
        ("attack", 0.3, None),
    ),
    "small": (("random-satellites", 0.01, 0),),
}
RESILIENCE_LANES = ("baseline", "reference")


class ExportError(Exception):
    """A program failed or its output is not what the export expects."""


# ---------------------------------------------------------------------------------------
# Running the programs.
# ---------------------------------------------------------------------------------------
def run_program(label: str, command: list[str], summary: Path, needs_passed: bool) -> float:
    """Runs one program; writes its standard output and error to summary. Raises ExportError
    if it exits non-zero or (needs_passed) its summary lacks "checks: PASSED"."""
    began = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    elapsed = time.monotonic() - began
    summary.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise ExportError(f"{label}: exit status {result.returncode}; see {summary}")
    if needs_passed and "checks: PASSED" not in result.stdout + result.stderr:
        raise ExportError(f"{label}: the summary does not say checks: PASSED; see {summary}")
    print(f"  {label}: {elapsed:.0f} s", flush=True)
    return elapsed


def failures_passed(label: str, summary: Path) -> None:
    parsed = r4.parse_summary(summary.read_text(encoding="utf-8"))
    if r4.run_failed({"exit": 0}, parsed):
        bad = {k: v for k, v in parsed["checks"].items() if v}
        raise ExportError(f"{label}: failed checks {bad}; see {summary}")


def parallel(jobs: int, calls: list) -> list:
    """Runs every (function, *arguments), at most jobs at a time; returns the results in
    order. The first exception is raised once every call has finished."""
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(f, *a) for f, *a in calls]
        concurrent.futures.wait(futures)
        return [f.result() for f in futures]


def propagate_command(exe: Path, duration_s: int, step_s: int, ids: list[int], out: Path):
    return [
        str(exe / "propagate"),
        "--snapshot",
        str(SNAPSHOT),
        "--start",
        WINDOW_START,
        "--duration-s",
        str(duration_s),
        "--step-s",
        str(step_s),
        "--ids",
        ",".join(str(i) for i in sorted(ids)),
        "--out",
        str(out),
    ]


def topology_command(exe: Path, pattern: str, res: Path) -> list[str]:
    return [
        str(exe / "topology"),
        "--snapshot",
        str(SNAPSHOT),
        "--ground",
        str(GROUND),
        "--cities",
        str(CITIES),
        "--roles",
        "study",
        "--start",
        PLAN_TIME,
        "--duration-s",
        "0",
        "--step-s",
        "1",
        "--pattern",
        pattern,
        "--model",
        "best-case",
        "--out",
        str(res / f"topology-{pattern}.csv"),
        "--links-out",
        str(res / f"links-{pattern}.csv"),
    ]


def routing_command(exe: Path, cities: Path, duration_s: int, res: Path) -> list[str]:
    """Phase 3's main settings (run_phase3.command), the best-case model."""
    run = r3.Run("dashboard", "main", ROUTE_MODEL, ROUTE_STRATEGIES, serving="")
    command = r3.command(
        run, exe / "routing", duration_s, res / "routing.routes.csv", res / "routing.drops.csv"
    )
    command[command.index("--cities") + 1] = str(cities)
    command[command.index("--start") + 1] = WINDOW_START
    # The shells, planes and laser plan of Phases 3 and 4 also when the window starts later.
    return [*command, "--plan-time", PLAN_TIME]


# ---------------------------------------------------------------------------------------
# Small helpers.
# ---------------------------------------------------------------------------------------
def read_csv(path: Path) -> list[dict]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def seconds_after(utc: str, start: str) -> float:
    return st.seconds_between(st.parse_utc(utc), st.parse_utc(start))


def whole(x: float) -> int | float:
    """A time that is a whole number of seconds as an int, so the JSON reads 67, not 67.0."""
    return int(x) if float(x).is_integer() else x


def number(text: str) -> float | None:
    """A decimal as the program wrote it; none, never and empty are null."""
    return None if text in ("", "none", "never") else float(text)


def count(text: str) -> int | None:
    return None if text in ("", "none", "never") else int(text)


def write_json(path: Path, data, pretty: bool = False) -> None:
    if pretty:
        text = json.dumps(data, indent=1, ensure_ascii=False, allow_nan=False)
    else:
        text = json.dumps(data, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    path.write_text(text + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------------------
# Satellites, sites and their indices.
# ---------------------------------------------------------------------------------------
class Index:
    """Satellite and site indices of the exported files."""

    def __init__(self, satellites: list[dict], sites: list[dict]):
        self.satellite = {s["norad_cat_id"]: i for i, s in enumerate(satellites)}
        self.site = {(s["kind"], s["name"]): i for i, s in enumerate(sites)}
        if len(self.site) != len(sites):
            raise ExportError("two ground sites share a kind and a name")

    def sat(self, norad: str | int) -> int:
        try:
            return self.satellite[int(norad)]
        except KeyError:
            raise ExportError(f"satellite {norad} is not in satellites.json") from None

    def sats(self, route: str) -> list[int]:
        return [self.sat(x) for x in route.split()]

    def place(self, kind: str, name: str) -> int:
        try:
            return self.site[(kind, name)]
        except KeyError:
            raise ExportError(f"{kind} {name!r} is not in sites.json") from None

    def node(self, text: str) -> list:
        """satellite:<n>, city:<name> or gateway:<name> -> [kind, index]."""
        kind, name = text.split(":", 1)
        if kind == "satellite":
            return ["satellite", self.sat(name)]
        if kind in ("city", "gateway"):
            return [kind, self.place(kind, name)]
        raise ExportError(f"unknown node {text!r}")

    def element(self, text: str) -> list | None:
        """A failed element (satellite:<n>, link:<a>-<b>, gateway:<name>) -> an array."""
        if text == "":
            return None
        kind, name = text.split(":", 1)
        if kind == "link":
            a, b = name.split("-")
            return ["link", self.sat(a), self.sat(b)]
        return self.node(text)


def satellites_of(constellation_csv: Path) -> list[dict]:
    """The satellites in planes, in catalogue order."""
    satellites = []
    for r in read_csv(constellation_csv):
        if not r["plane_index"]:
            continue
        satellites.append(
            {
                "norad_cat_id": int(r["norad_cat_id"]),
                "name": r["object_name"],
                "shell": r["shell"],
                "plane": int(r["plane_index"]),
                "index_in_plane": int(r["index_in_plane"]),
            }
        )
    satellites.sort(key=lambda s: s["norad_cat_id"])
    return satellites


def sites_of() -> list[dict]:
    """The cities of the study pairs, every gateway (real, then assumed), every PoP."""
    cities = json.loads(CITIES.read_text(encoding="utf-8"))
    study = {n for p in cities["pairs"] if p["role"] == "study" for n in (p["from"], p["to"])}
    sites = [
        {
            "name": c["name"],
            "kind": "city",
            "lat_deg": c["lat"],
            "lon_deg": c["lon"],
            "country": c["country"],
        }
        for c in cities["cities"]
        if c["name"] in study
    ]
    for file, source in (("gateways.json", "real"), ("assumed_gateways.json", "assumed")):
        for g in json.loads((GROUND / file).read_text(encoding="utf-8")):
            sites.append(
                {
                    "name": g["name"],
                    "kind": "gateway",
                    "lat_deg": g["lat"],
                    "lon_deg": g["lon"],
                    "status": g["status"],
                    "source": source,
                }
            )
    for p in json.loads((GROUND / "pop_codes.json").read_text(encoding="utf-8")):
        sites.append(
            {
                "name": p["code"],
                "kind": "pop",
                "lat_deg": p["lat"],
                "lon_deg": p["lon"],
                "city": p["city"],
                "country": p["country"],
            }
        )
    return sites


# ---------------------------------------------------------------------------------------
# Positions.
# ---------------------------------------------------------------------------------------
def positions_array(path: Path, index: Index, samples: int) -> np.ndarray:
    """(samples, satellites, 3) ECEF km as float32; NaN where a satellite has no row."""
    out = np.full((samples, len(index.satellite), 3), np.nan, dtype=np.float32)
    with path.open(newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            t = seconds_after(r["utc"], WINDOW_START)
            k = round(t / SAMPLE_STEP_S)
            if k * SAMPLE_STEP_S != t or not 0 <= k < samples:
                raise ExportError(f"{path}: a row at {r['utc']} is not a sample time")
            out[k, index.sat(r["norad_cat_id"])] = (
                float(r["ecef_x_km"]),
                float(r["ecef_y_km"]),
                float(r["ecef_z_km"]),
            )
    return out


def write_positions(out_dir: Path, positions: np.ndarray) -> list[dict]:
    """Cut into files under MAX_FILE_BYTES, whole samples each."""
    per_sample = positions.shape[1] * 3 * 4
    per_file = (MAX_FILE_BYTES - 1) // per_sample
    entries = []
    for n, first in enumerate(range(0, positions.shape[0], per_file)):
        chunk = positions[first : first + per_file]
        name = f"positions-{n}.bin"
        (out_dir / name).write_bytes(chunk.astype("<f4").tobytes())
        entries.append({"path": name, "first_sample": first, "samples": int(chunk.shape[0])})
    return entries


# ---------------------------------------------------------------------------------------
# Laser links and the constellation story.
# ---------------------------------------------------------------------------------------
def links_of(path: Path, index: Index) -> list[list[int]]:
    """The links of a --links-out file, in file order, as satellite indices."""
    links, header = [], None
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        if header is None:
            header = line
            continue
        a, b = line.split(",")[:2]
        links.append([index.sat(a), index.sat(b)])
    return links


_SHELL = re.compile(
    r"^  shell (\S+) \(inclination ([\d.]+) \+- ([\d.]+) deg, height ([\d.]+) to ([\d.]+) km, "
    r"plane gap ([\d.]+) deg, planes of (\d+) or more\): (\d+) satellites in the bands, "
    r"(\d+) in planes, (\d+) planes, plane size min (\d+) median ([\d.]+) max (\d+)$",
    re.M,
)
_PLANNED = re.compile(r"^  planned laser links: (\d+)\n((?:    [a-z_]+: \d+\n)*)", re.M)


def shell_rows(summary: Path) -> list[dict]:
    """The shells of the constellation summary, in its order."""
    rows = []
    for m in _SHELL.finditer(summary.read_text(encoding="utf-8")):
        g = m.groups()
        rows.append(
            {
                "shell": g[0],
                "inclination_deg": float(g[1]),
                "inclination_tolerance_deg": float(g[2]),
                "height_min_km": float(g[3]),
                "height_max_km": float(g[4]),
                "plane_gap_deg": float(g[5]),
                "min_plane_size": int(g[6]),
                "satellites_in_bands": int(g[7]),
                "satellites_in_planes": int(g[8]),
                "planes": int(g[9]),
                "plane_size_min": int(g[10]),
                "plane_size_median": float(g[11]),
                "plane_size_max": int(g[12]),
            }
        )
    if not rows:
        raise ExportError(f"{summary}: no shell lines")
    return rows


def constellation_story(summary: Path, topology: dict[str, Path]) -> dict:
    patterns = []
    for pattern in PATTERNS:
        m = _PLANNED.search(topology[pattern].read_text(encoding="utf-8"))
        if not m:
            raise ExportError(f"{topology[pattern]}: no planned laser links line")
        kinds = {k: int(v) for k, v in re.findall(r"    ([a-z_]+): (\d+)", m.group(2))}
        patterns.append(
            {
                "pattern": pattern,
                "links": int(m.group(1)),
                "by_kind": kinds,
                "file": f"links-{pattern}.json",
            }
        )
    return {
        "sources": {"shells": summary.name, "patterns": [p.name for p in topology.values()]},
        "plan_time_utc": PLAN_TIME,
        "shells": shell_rows(summary),
        "patterns": patterns,
    }


# ---------------------------------------------------------------------------------------
# The route story.
# ---------------------------------------------------------------------------------------
def drop_element(r: dict, index: Index) -> list | None:
    if r["satellite_norad_cat_id"]:
        return ["satellite", index.sat(r["satellite_norad_cat_id"])]
    if r["link_norad_cat_id_a"]:
        return [
            "link",
            index.sat(r["link_norad_cat_id_a"]),
            index.sat(r["link_norad_cat_id_b"]),
            r["link_down_reason"],
        ]
    if r["site"]:
        return [r["site_kind"], index.place(r["site_kind"], r["site"])]
    return None


def route_story(res: Path, index: Index, pairs: list[tuple[str, str]], duration_s: int) -> dict:
    routes = read_csv(res / "routing.routes.csv")
    drops = read_csv(res / "routing.drops.csv")
    summary = r3.parse_summary((res / "routing.summary.txt").read_text(encoding="utf-8"))
    directions = [d for p in pairs for d in (p, (p[1], p[0]))]
    steps = duration_s + 1
    lanes: dict[tuple, list[dict]] = {}
    for r in routes:
        lanes.setdefault((r["from"], r["to"], r["strategy"]), []).append(r)
    out = []
    for d in directions:
        strategies = {}
        for s in ROUTE_STRATEGIES:
            rows = lanes.get((*d, s), [])
            if len(rows) != steps:
                raise ExportError(f"routing: {d} {s}: {len(rows)} rows, expected {steps}")
            delays, intervals = [], []
            for k, r in enumerate(rows):
                if seconds_after(r["utc"], WINDOW_START) != k:
                    raise ExportError(f"routing: {d} {s}: row {k} at {r['utc']}")
                ok = r["status"] == "ok"
                delays.append(float(r["one_way_ms"]) if ok else None)
                if not ok:
                    continue
                route = index.sats(r["route"])
                gateway = index.place("gateway", r["gateway"]) if r["gateway"] else None
                last = intervals[-1] if intervals else None
                if last and last[1] == k - 1 and last[2] == route and last[3] == gateway:
                    last[1] = k
                else:
                    intervals.append([k, k, route, gateway])
            lane_drops = [
                [
                    whole(seconds_after(x["start_utc"], WINDOW_START)),
                    whole(float(x["length_s"])),
                    x["cause"],
                    drop_element(x, index),
                ]
                for x in drops
                if (x["from"], x["to"], x["strategy"]) == (*d, s)
            ]
            lane = summary["directions"][d]["strategies"][s]
            delay = lane.get("delay")
            strategies[s] = {
                "intervals": intervals,
                "delay_ms": delays,
                "drops": lane_drops,
                "summary": {
                    "ok_steps": lane["ok"],
                    "steps": lane["steps"],
                    "delay_median_ms": delay[2] if delay else None,
                    "bad_changes": lane["bad"],
                    "increases": lane["increases"],
                    "changes": lane["bad"] + lane["increases"],
                    "breaks": lane["breaks"],
                    "recoveries": lane["recoveries"],
                    "drop_events": lane["drop_events"],
                    "no_route_s": lane["drop_s"],
                    "no_route_total_s": sum(lane["drop_s"].values()),
                },
            }
        out.append(
            {
                "from": index.place("city", d[0]),
                "to": index.place("city", d[1]),
                "strategies": strategies,
            }
        )
    return {
        "sources": {
            "routes": "routing.routes.csv",
            "drops": "routing.drops.csv",
            "summary": "routing.summary.txt",
        },
        "settings": {
            "model": ROUTE_MODEL,
            "pattern": "three",
            "replan_s": 15,
            "planner": "start",
            "backup": "on",
            "step_s": 1,
            "start_utc": WINDOW_START,
            "duration_s": duration_s,
            "strategies": list(ROUTE_STRATEGIES),
        },
        "run_start_s": whole(seconds_after(WINDOW_START, WINDOW_START)),
        "directions": out,
    }


# ---------------------------------------------------------------------------------------
# The failure story.
# ---------------------------------------------------------------------------------------
def failure_paths(res: Path, scenario: str) -> dict[str, Path]:
    base = res / f"failure-{scenario}"
    return {
        k: Path(f"{base}.{k}.csv")
        for k in ("routes", "events", "facts", "floods", "measures", "network", "links")
    } | {"summary": Path(f"{base}.summary.txt"), "failures": Path(f"{base}.failures.csv")}


def failure_command(exe: Path, cities: Path, p: dict[str, Path]) -> list[str]:
    start, duration = r4.window(FAILURE_INSTANT)
    command = r4.common_arguments(
        exe / "failures", cities, "study", FAILURE_MODEL, r4.utc_at(start), duration
    )
    return [
        *command,
        *r4.Settings().arguments(),
        "--failures",
        str(p["failures"]),
        "--out",
        str(p["routes"]),
        "--events-out",
        str(p["events"]),
        "--facts-out",
        str(p["facts"]),
        "--floods-out",
        str(p["floods"]),
        "--measures-out",
        str(p["measures"]),
        "--network-out",
        str(p["network"]),
        "--links-out",
        str(p["links"]),
    ]


def failed_elements(failures: tuple[mf.Failure, ...], index: Index) -> dict:
    out = {"satellites": [], "links": [], "gateways": []}
    for f in failures:
        if f.kind == "satellite":
            out["satellites"].append(index.sat(f.a))
        elif f.kind == "laser_link":
            out["links"].append([index.sat(f.a), index.sat(f.b)])
        else:
            out["gateways"].append(index.place("gateway", f.gateway))
    return out


def facts_of(rows: list[dict], index: Index) -> list[dict]:
    facts = []
    for r in rows:
        detectors = []
        for d in r["detectors"].split(";") if r["detectors"] else []:
            node, at = d.rsplit("@", 1)
            detectors.append([*index.node(node), float(at)])
        facts.append(
            {
                "fact": int(r["fact"]),
                "subject": index.element(r["subject"]),
                "state": r["state"],
                "t_s": float(r["t_s"]),
                "old": r["old"] == "yes",
                "detectors": detectors,
                "planner_known_s": number(r["planner_known_s"]),
                "reports": count(r["reports"]),
                "report_hops": count(r["report_hops"]),
                "flood_nodes": count(r["flood_nodes"]),
                "flood_last_s": number(r["flood_last_s"]),
                "messages": count(r["messages"]),
            }
        )
    return facts


def first_learned(path: Path, index: Index, sites: int) -> dict:
    """Per satellite and per site, the first instant it learned any fact (null: never)."""
    satellites: list[float | None] = [None] * len(index.satellite)
    places: list[float | None] = [None] * sites
    with path.open(newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            kind, i = index.node(r["node"])
            target = satellites if kind == "satellite" else places
            t = float(r["learn_s"])
            if target[i] is None or t < target[i]:
                target[i] = t
    return {"satellite_first_learn_s": satellites, "site_first_learn_s": places}


MEASURE_NUMBERS = (
    "failure_s",
    "black_hole_s",
    "loop_s",
    "lost_s",
    "restore_s",
    "settle_s",
    "extra_min_ms",
    "extra_median_ms",
    "extra_max_ms",
    "extra_settled_ms",
)


def slow_motion(failure_s: float, end_s: float) -> list[dict]:
    first_end = failure_s + FIRST_SPAN_S
    spans = [{"from_s": whole(failure_s), "to_s": whole(first_end), "speed": FIRST_SPAN_SPEED}]
    if end_s > first_end:
        spans.append({"from_s": whole(first_end), "to_s": whole(end_s), "speed": SETTLE_SPAN_SPEED})
    return spans


def failure_scenario(
    res: Path, scenario: str, run: r4.HandRun, failures, index: Index, sites: int
) -> dict:
    p = failure_paths(res, scenario)
    start, duration = r4.window(FAILURE_INSTANT)
    lanes: dict[str, list] = {lane: [] for lane in LANES}
    for e in read_csv(p["events"]):
        if (e["from"], e["to"]) != FAILURE_PAIR:
            raise ExportError(f"{p['events']}: a row of another direction")
        lanes[e["reaction"]].append(
            [
                whole(float(e["start_s"])),
                whole(float(e["end_s"])),
                e["state"],
                e["drop_cause"] or None,
                index.element(e["failed_element"]),
                number(e["one_way_ms"]),
                index.sats(e["route"]),
                index.sat(e["serving_norad_cat_id"]) if e["serving_norad_cat_id"] else None,
                index.place("gateway", e["gateway"]) if e["gateway"] else None,
            ]
        )
    plans: dict[str, list] = {lane: [] for lane in LANES}
    home_pop = None
    for r in read_csv(p["routes"]):
        if r["plan"] and r["reaction"] in plans:
            plans[r["reaction"]].append(
                [whole(seconds_after(r["utc"], r4.utc_at(start))), r["plan"]]
            )
        if r["home_pop"]:
            home_pop = index.place("pop", r["home_pop"])
    measures = {}
    for r in read_csv(p["measures"]):
        measures[r["reaction"]] = {k: number(r[k]) for k in MEASURE_NUMBERS} | {
            "reports": count(r["reports"]),
            "messages": count(r["messages"]),
        }
    if sorted(measures) != sorted(r4.ALL_REACTIONS):
        raise ExportError(f"{p['measures']}: reactions {sorted(measures)}")
    failure_s = measures["planner"]["failure_s"]
    settles = [m["settle_s"] for m in measures.values()]
    end = duration if None in settles else failure_s + max(settles)
    context = [
        {
            "run": r["run"],
            "model": r["model"],
            "from": r["from"],
            "to": r["to"],
            "instant": int(r["instant"]),
            "reaction": r["reaction"],
            "lost_s": number(r["lost_s"]),
        }
        for r in read_csv(PHASE4_DATA / "measures.csv")
        if r["group"] == "main" and r["scenario"] == scenario and r["variant"] == "default"
    ]
    return {
        "scenario": scenario,
        "sources": {k: v.name for k, v in p.items()},
        "start_utc": r4.utc_at(start),
        "duration_s": duration,
        "run_start_s": whole(seconds_after(r4.utc_at(start), WINDOW_START)),
        "failure_s": whole(failure_s),
        "failed": failed_elements(failures, index),
        "home_pop": home_pop,
        "lanes": lanes,
        "plans": plans,
        "measures": measures,
        "facts": facts_of(read_csv(p["facts"]), index),
        "flood": first_learned(p["floods"], index, sites),
        "slow_motion": slow_motion(failure_s, end),
        "pause_before_failure_real_s": PAUSE_BEFORE_FAILURE_REAL_S,
        "context": {
            "source": "docs/phase-4-data/measures.csv",
            "this_run": run.name,
            "rows": context,
        },
    }


# ---------------------------------------------------------------------------------------
# The falling-apart story.
# ---------------------------------------------------------------------------------------
def committed_table(path: Path) -> dict:
    """A committed summary CSV, cells as numbers where they read as one, empty as null."""
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        columns = next(reader)
        rows = []
        for line in reader:
            cells = []
            for text in line:
                if text == "":
                    cells.append(None)
                    continue
                try:
                    value = float(text)
                except ValueError:
                    cells.append(text)
                    continue
                cells.append(int(text) if re.fullmatch(r"-?\d+", text) else value)
            rows.append(cells)
    return {"source": str(path.relative_to(REPO)), "columns": columns, "rows": rows}


def resilience_name(scenario: str, share: float, seed: int | None) -> str:
    which = f"seed-{seed}" if seed is not None else f"T{RESILIENCE_INSTANT}"
    return f"{scenario}.share-{share:g}.{which}"


def resilience_paths(res: Path, name: str) -> dict[str, Path]:
    base = res / f"resilience-{name}"
    return {
        k: Path(f"{base}.{k}.csv")
        for k in ("routes", "facts", "measures", "network", "links", "failures")
    } | {"summary": Path(f"{base}.summary.txt")}


def resilience_command(exe: Path, p: dict[str, Path]) -> list[str]:
    at = r4.utc_at(r4.instant_offset_s(RESILIENCE_INSTANT))
    command = r4.common_arguments(exe / "failures", CITIES, "study", "best-case", at, 0)
    settings = r4.RecoveryRun("", "", "three", 0, None, RESILIENCE_INSTANT, "off").settings
    return [
        *command,
        *settings.arguments(),
        "--failures",
        str(p["failures"]),
        "--out",
        str(p["routes"]),
        "--facts-out",
        str(p["facts"]),
        "--measures-out",
        str(p["measures"]),
        "--network-out",
        str(p["network"]),
        "--links-out",
        str(p["links"]),
    ]


def resilience_step(res: Path, step: tuple, failures, index: Index, planned: list) -> dict:
    scenario, share, seed = step
    name = resilience_name(*step)
    p = resilience_paths(res, name)
    routes = []
    lanes: dict[tuple, dict] = {}
    for r in read_csv(p["routes"]):
        if r["reaction"] in RESILIENCE_LANES:
            lanes[(r["from"], r["to"], r["reaction"])] = r
    for from_, to in r4.study_pairs():
        entry = {"from": index.place("city", from_), "to": index.place("city", to)}
        for lane in RESILIENCE_LANES:
            r = lanes[(from_, to, lane)]
            entry[lane] = {
                "state": r["state"],
                "drop_cause": r["drop_cause"] or None,
                "one_way_ms": number(r["one_way_ms"]),
                "route": index.sats(r["route"]),
            }
        routes.append(entry)
    if links_of(p["links"], index) != planned:
        raise ExportError(f"{p['links']}: not the laser plan of links-three.csv")
    failed_links = [
        index.element(f["subject"])[1:]
        for f in read_csv(p["facts"])
        if f["subject"].startswith("link:") and f["state"] == "down"
    ]
    down = {tuple(sorted(link)) for link in failed_links}
    network = [
        {
            "scope": r["scope"],
            "working_satellites": int(r["working_satellites"]),
            "pieces": int(r["pieces"]),
            "largest_piece": int(r["largest_piece"]),
            "largest_piece_share": number(r["largest_piece_share"]),
        }
        for r in read_csv(p["network"])
    ]
    at = r4.instant_offset_s(RESILIENCE_INSTANT)
    at_utc = r4.utc_at(at)
    return {
        "name": name,
        "sources": {k: v.name for k, v in p.items()},
        "scenario": scenario,
        "share": share,
        "seed": seed,
        "instant": RESILIENCE_INSTANT,
        "utc": at_utc,
        "t_s": whole(seconds_after(at_utc, WINDOW_START)),
        "pattern": "three",
        "repoint": "off",
        "failed_satellites": failed_elements(failures, index)["satellites"],
        "failed_links": failed_links,
        "planned_links_not_failed": [
            i for i, link in enumerate(planned) if tuple(sorted(link)) not in down
        ],
        "routes": routes,
        "network": network,
    }


# ---------------------------------------------------------------------------------------
def use_mode(mode: str) -> None:
    """Sets the window start and the one-step instant of an export: full, the plan time; small,
    the failure story's run start, so that every story's times lie inside the window."""
    global WINDOW_START, RESILIENCE_INSTANT
    WINDOW_START = PLAN_TIME if mode == "full" else r4.utc_at(r4.window(FAILURE_INSTANT)[0])
    RESILIENCE_INSTANT = RESILIENCE_INSTANTS[mode]


WINDOW_START = PLAN_TIME
RESILIENCE_INSTANT = RESILIENCE_INSTANTS["full"]


def export(args, mode: str) -> dict:
    use_mode(mode)
    exe, out, res = args.bin, args.out, args.results / mode
    for program in ("propagate", "constellation", "topology", "routing", "failures"):
        if not (exe / program).is_file():
            raise ExportError(f"{exe / program} not found")
    inputs = res / "inputs"
    inputs.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    window_s = WINDOW_S[mode]
    samples = window_s // SAMPLE_STEP_S + 1

    print("constellation", flush=True)
    constellation_csv = res / "constellation.csv"
    run_program(
        "constellation",
        [
            str(exe / "constellation"),
            "--snapshot",
            str(SNAPSHOT),
            "--time",
            PLAN_TIME,
            "--out",
            str(constellation_csv),
        ],
        res / "constellation.summary.txt",
        needs_passed=False,
    )
    satellites = satellites_of(constellation_csv)
    shells = [r["shell"] for r in shell_rows(res / "constellation.summary.txt")]
    if {s["shell"] for s in satellites} - set(shells):
        raise ExportError("a satellite in a plane has a shell the summary does not list")
    sites = sites_of()
    index = Index(satellites, sites)
    planes = mf.read_planes(constellation_csv)
    ids = [s["norad_cat_id"] for s in satellites]

    route_pairs = r4.study_pairs() if mode == "full" else [ROUTE_SMALL_PAIR]
    route_cities = CITIES
    if mode == "small":
        route_cities = inputs / f"pair.{r4.slug(ROUTE_SMALL_PAIR)}.json"
        r4.write_pair_cities(route_cities, ROUTE_SMALL_PAIR)
    failure_cities = inputs / f"pair.{r4.slug(FAILURE_PAIR)}.json"
    r4.write_pair_cities(failure_cities, FAILURE_PAIR)
    baselines = res / "baselines"
    baselines.mkdir(exist_ok=True)
    resilience = RESILIENCE_STEPS[mode]
    attack = any(s[0] == "attack" for s in resilience)

    def baseline_failure():
        _, status = r4.baseline_hand(
            exe / "failures", baselines, ("whole-interval", FAILURE_MODEL, FAILURE_INSTANT), False
        )
        if status["exit"] != 0:
            raise ExportError(f"failure baseline: exit status {status['exit']}")

    def baseline_attack():
        pairs = len(json.loads(r4.HYPATIA_CITIES.read_text())["pairs"])
        _, status = r4.baseline_recovery(
            exe / "failures", baselines, r4.HYPATIA_CITIES, pairs, RESILIENCE_INSTANT, False
        )
        if status["exit"] != 0:
            raise ExportError(f"attack baseline: exit status {status['exit']}")

    print("first runs", flush=True)
    calls = [
        (
            run_program,
            "propagate (samples)",
            propagate_command(exe, window_s, SAMPLE_STEP_S, ids, res / "positions.csv"),
            res / "positions.summary.txt",
            False,
        ),
        (
            run_program,
            "propagate (1 s)",
            propagate_command(
                exe, ERROR_WINDOW_S[mode], ERROR_STEP_S, ids, res / "positions-1s.csv"
            ),
            res / "positions-1s.summary.txt",
            False,
        ),
        (
            run_program,
            "routing",
            routing_command(exe, route_cities, window_s, res),
            res / "routing.summary.txt",
            True,
        ),
        (baseline_failure,),
        *[
            (
                run_program,
                f"topology {p}",
                topology_command(exe, p, res),
                res / f"topology-{p}.summary.txt",
                True,
            )
            for p in PATTERNS
        ],
    ]
    if attack:
        calls.append((baseline_attack,))
    parallel(args.jobs, calls)

    # The failure scenarios, from the baseline route at T1 (run_phase4.main).
    key = ("whole-interval", FAILURE_MODEL, FAILURE_INSTANT)
    bp = r4.Paths(baselines, f"hand.{key[0]}.{key[1]}.T{key[2]}")
    text = bp.summary.read_text(encoding="utf-8")
    if r4.run_failed(json.loads(bp.status.read_text()), r4.parse_summary(text)):
        raise ExportError(f"{bp.summary}: the failure baseline run failed")
    if FAILURE_PAIR[0] in r4.no_home_pop(text):
        raise ExportError(f"{FAILURE_PAIR[0]} has no home PoP")
    at = r4.utc_at(r4.instant_offset_s(FAILURE_INSTANT))
    route = mf.baseline_routes(r4.read_csv(bp.rows), at).get(FAILURE_PAIR)

    def points():
        return mf.subsatellite_points(
            exe / "propagate", SNAPSHOT, at, sorted(planes), inputs / "points.csv"
        )

    hand: dict[str, tuple] = {}
    calls = []
    for scenario in FAILURE_SCENARIOS[mode]:
        run = r4.HandRun(
            "main", scenario, "default", FAILURE_MODEL, FAILURE_PAIR, FAILURE_INSTANT, r4.Settings()
        )
        chosen = mf.hand_picked(scenario, route, FAILURE_MODEL, at, planes, points)
        if chosen.skipped:
            raise ExportError(f"failure scenario {scenario}: skipped ({chosen.skipped})")
        p = failure_paths(res, scenario)
        p["failures"].write_text(
            r4.failure_file_text(
                chosen.failures,
                [
                    f"Dashboard failure story: Phase 4 scenario {scenario}, {FAILURE_PAIR[0]} -> "
                    f"{FAILURE_PAIR[1]}, {FAILURE_MODEL}, T{FAILURE_INSTANT} = {at}, permanent"
                ],
            ),
            encoding="utf-8",
        )
        hand[scenario] = (run, chosen.failures)
        calls.append(
            (
                run_program,
                f"failures {scenario}",
                failure_command(exe, failure_cities, p),
                p["summary"],
                True,
            )
        )

    # The one-step runs (run_phase4.main, after recovery).
    step_at = r4.utc_at(r4.instant_offset_s(RESILIENCE_INSTANT))
    dated = mf.shifted_utc(step_at, -mf.OLD_NEWS_S)
    load = None
    if attack:
        pairs = len(json.loads(r4.HYPATIA_CITIES.read_text())["pairs"])
        rows = r4.read_csv(
            r4.Paths(baselines, r4.recovery_baseline_name(pairs, RESILIENCE_INSTANT)).rows
        )
        load = mf.route_load(mf.baseline_routes(rows, step_at).values())
    steps: dict[str, tuple] = {}
    for step in resilience:
        scenario, share, seed = step
        if scenario == "random-satellites":
            chosen = mf.random_satellites(planes, share, seed)
        else:
            chosen = mf.attack_satellites(planes, load, share)
        failures = mf.satellite_failures(chosen, dated)
        name = resilience_name(*step)
        p = resilience_paths(res, name)
        p["failures"].write_text(
            r4.failure_file_text(
                failures,
                [
                    f"Dashboard falling-apart story: {scenario}, share {share:g} of {len(planes)} "
                    f"satellites in planes, {len(chosen)} failed, seed {seed}",
                    f"step T{RESILIENCE_INSTANT} = {step_at}; failures dated "
                    f"{mf.OLD_NEWS_S} s before it",
                ],
            ),
            encoding="utf-8",
        )
        steps[name] = (step, failures)
        calls.append(
            (run_program, f"failures {name}", resilience_command(exe, p), p["summary"], True)
        )
    print("failure runs", flush=True)
    parallel(args.jobs, calls)
    for scenario in hand:
        failures_passed(f"failures {scenario}", failure_paths(res, scenario)["summary"])
    for name in steps:
        failures_passed(f"failures {name}", resilience_paths(res, name)["summary"])

    # The exported files.
    print("writing", flush=True)
    for old in [*out.glob("*.json"), *out.glob("positions-*.bin")]:
        old.unlink()
    positions = write_positions(out, positions_array(res / "positions.csv", index, samples))
    write_json(
        out / "satellites.json",
        {
            "shells": shells,
            "norad_cat_id": [s["norad_cat_id"] for s in satellites],
            "name": [s["name"] for s in satellites],
            "shell": [shells.index(s["shell"]) for s in satellites],
            "plane": [s["plane"] for s in satellites],
            "index_in_plane": [s["index_in_plane"] for s in satellites],
        },
    )
    write_json(out / "sites.json", {"sites": sites}, pretty=True)
    links_three = links_of(res / "links-three.csv", index)
    for pattern in PATTERNS:
        write_json(
            out / f"links-{pattern}.json",
            {"pattern": pattern, "links": links_of(res / f"links-{pattern}.csv", index)},
        )
    write_json(
        out / "story-constellation.json",
        constellation_story(
            res / "constellation.summary.txt",
            {p: res / f"topology-{p}.summary.txt" for p in PATTERNS},
        ),
        pretty=True,
    )
    write_json(out / "story-route.json", route_story(res, index, route_pairs, window_s))
    write_json(
        out / "story-failure.json",
        {
            "settings": {
                "model": FAILURE_MODEL,
                "pattern": "three",
                "strategy": r4.STRATEGY,
                "step_s": r4.STEP_S,
                "replan_s": r4.REPLAN_S,
                "planner": r4.Settings().planner,
                "detection_s": r4.Settings().detection_s,
                "hop_processing_s": r4.Settings().hop_processing_s,
                "compute_s": r4.Settings().compute_s,
                "planner_processing_s": r4.Settings().planner_processing_s,
                "acquisition_s": r4.Settings().acquisition_s,
                "repoint": "off",
                "replan_on_report": "off",
            },
            "from": index.place("city", FAILURE_PAIR[0]),
            "to": index.place("city", FAILURE_PAIR[1]),
            "instant": FAILURE_INSTANT,
            "instant_utc": at,
            "scenarios": [
                failure_scenario(res, s, run, failures, index, len(sites))
                for s, (run, failures) in hand.items()
            ],
        },
    )
    write_json(
        out / "story-resilience.json",
        {
            "charts": {
                "recovery_summary": committed_table(PHASE4_DATA / "recovery_summary.csv"),
                "percolation": committed_table(PHASE4_DATA / "percolation.csv"),
            },
            "steps": [
                resilience_step(res, step, failures, index, links_three)
                for step, failures in steps.values()
            ],
        },
    )

    files = []
    for path in sorted(out.iterdir()):
        if path.name == "manifest.json":
            continue
        data = path.read_bytes()
        if len(data) >= MAX_FILE_BYTES:
            raise ExportError(f"{path} is {len(data)} bytes, the limit is under {MAX_FILE_BYTES}")
        files.append(
            {"path": path.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        )
    manifest = {
        "format_version": FORMAT_VERSION,
        "snapshot": SNAPSHOT.name,
        "ground": GROUND.name,
        "settings": {
            "mode": mode,
            "plan_time_utc": PLAN_TIME,
            "window_start_utc": WINDOW_START,
            "window_s": window_s,
            "sample_step_s": SAMPLE_STEP_S,
            "samples": samples,
            "error_window_s": ERROR_WINDOW_S[mode],
            "error_step_s": ERROR_STEP_S,
        },
        "satellites": len(satellites),
        "positions": positions,
        "files": files,
    }
    write_json(out / "manifest.json", manifest, pretty=True)
    return manifest


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Export the dashboard's data.")
    parser.add_argument("--small", action="store_true", help="the seven-minute export")
    parser.add_argument("--bin", type=Path, default=DEFAULT_BIN, help="the built programs")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="exported data")
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS, help="engine CSVs")
    parser.add_argument("--jobs", type=int, default=4, help="programs at the same time")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        print("error: --jobs must be at least 1", file=sys.stderr)
        return 2
    mode = "small" if args.small else "full"
    began = time.monotonic()
    try:
        manifest = export(args, mode)
    except (ExportError, SystemExit) as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1
    total = sum(f["bytes"] for f in manifest["files"])
    for f in manifest["files"]:
        print(f"  {f['path']}: {f['bytes']:,} bytes")
    print(
        f"exported {mode} data to {args.out}: {len(manifest['files'])} files, {total:,} bytes, "
        f"{time.monotonic() - began:.0f} s"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

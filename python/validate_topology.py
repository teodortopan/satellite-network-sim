#!/usr/bin/env python3
"""Cross-check the topology program on Hypatia's Starlink shell against an independent model.

Spec section 6.2. Writes Hypatia's first Starlink shell (python/make_ideal_shell.py: 72 planes
of 22 satellites, 53 deg, their elements and phasing) as a snapshot, runs the C++ programs on
it, and recomputes the best case ("Starlink as a backbone") here, sharing no code with them:

  positions   python-sgp4 from the elements BY CONSTRUCTION (plane and slot -> elements,
              make_ideal_shell.Slot), not read back from the files; TEME to ITRS with
              Skyfield's own frame code, UT1 and polar motion from the same finals2000A.all.
              Times are built from calendar fields, never one float Julian date.
  lasers      +Grid BY CONSTRUCTION from plane and slot indices: slot s of plane o links to
              slots s +- 1 of plane o and to one slot of planes o +- 1 (planes wrap). Up when
              the segment clears 80 km above a sphere of radius 6378.137 km.
  ground      a city (cities.json, height 0 on the WGS-84 ellipsoid) may use any satellite at
              or above 25 deg elevation, measured from the ellipsoid normal.
  path        Dijkstra written here; delay = path length / c.

Which slot of the next plane: two rules are built.
  "nearest"   the slot whose argument of latitude is closest to u - dOmega * cos(i), where u is
              the satellite's own and dOmega = 5 deg the RAAN step (to first order, rotating a
              plane by dOmega about the pole moves a point along the orbit by dOmega * cos i).
              Computed from the indices and the phasing formula alone. This is the rule the
              program's nearest-distance matching should reproduce, and the one compared
              against it (gating).
  "hypatia"   the same slot index (Hypatia's generate_plus_grid_isls, isl_shift = 0). With
              Hypatia's half-slot phasing of odd planes, this is the nearer slot only from odd
              to even planes; from even to odd planes it is the farther diagonal. Reported, to
              say exactly how the two grids differ and what that does to the delay.
A Python greedy nearest-distance matching at the start time (the program's documented rule,
spec 4.3, written again) is also reported against both, as the explanation.

Checks (exit 1 if any fails):
  1. constellation --shells-file: every satellite in shell hypatia-s1, at the plane and index
     its construction gives; 72 planes of 22; each element set from the file the documented
     rule picks (plane 0 standard, the others supplemental: make_ideal_shell's decoys).
  2. topology --pattern plus-grid --model best-case: 1,584 satellites in 72 planes, 3,168
     planned laser links (1,584 in plane, 1,584 cross plane, so 4 per satellite), no laser
     without a link, no link down, no failed physical check.
  3. Every row (pair, direction, time) against the "nearest" model: the same status, laser
     hops, first and last satellite, and a one-way delay within TOLERANCE_MS.
  4. Hypatia's published round-trip times (HYPATIA_PUBLISHED) against this model built with
     Hypatia's own same-index grid and the 25 deg mask, within 20 % (spec 6.2). The program's
     values, on its nearest-slot grid, are printed next to them but not held to Hypatia's: on
     this shell the two grids are different link sets, and the program's gives much shorter
     north-south paths (docs/phase-2-results.md).

Usage (inside the CI image):

    python3 python/validate_topology.py --topology build/validate/bin/topology \\
        --constellation build/validate/bin/constellation

Reads only local files; any network access raises.
"""

from __future__ import annotations

import argparse
import csv
import heapq
import json
import math
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from sgp4.api import WGS72, Satrec, SatrecArray
from skyfield.api import wgs84
from skyfield.framelib import itrs
from skyfield.sgp4lib import TEME

import make_ideal_shell as ideal
import snapshot_tools as st
from validate_against_reference import skyfield_timescale

C_KM_S = 299_792.458
EQUATORIAL_RADIUS_KM = 6378.137
ATMOSPHERE_KM = 80.0
# The FCC mask for satellites above 500 km (spec 4.4); the shell's mean height is 515.6 km.
MASK_DEG = 25.0
SGP4_EPOCH_JD = 2433281.5  # 1949 December 31 0h, the zero of sgp4init's epoch argument.

# One-way delay tolerance per row. Both sides use the same SGP4 and Earth-orientation file;
# Phase 1 measured their Earth-fixed positions 0.43 mm apart at most (Skyfield's TIO locator).
# A path here has at most about 40 links, each end of each link off by up to 0.43 mm, so a
# path differs by at most about 3.5 cm, 1.2e-7 ms. The tolerance is about ten times that.
# Measured: see docs/phase-2-results.md. Anything that changes a link, a satellite or a
# ground link changes a path by tens of kilometres (0.1 ms or more).
TOLERANCE_MS = 1e-6

# The main cross-check: one hour from the shell's epoch, every minute, every pair.
MAIN_START = "2000-01-01T00:00:00"
MAIN_DURATION_S = 3600
MAIN_STEP_S = 60
# Hypatia's runs: 200 s from the epoch; theirs every 100 ms, here every second.
HYPATIA_DURATION_S = 200
HYPATIA_STEP_S = 1

# Every round-trip time the Hypatia paper or repository prints for a pair of cities on the
# Starlink first shell (searched on 2026-11-19; the paper's other per-pair values, section
# 4.1, are for Kuiper K1). Pair, (shortest, longest) round trip in ms over their 200 s run,
# and where it is printed.
HYPATIA_PUBLISHED = [
    (
        ("Paris", "Luanda"),
        (85.0, 117.0),
        "Kassing et al., IMC 2020, section 6 and Fig. 13 caption ('the Paris-Luanda RTT "
        "varies between 117 ms (left) and 85 ms'); repository README.md, the Paris-Luanda "
        "figure ('Left: 117 ms, Right: 85 ms')",
    ),
]
HYPATIA_TOLERANCE = 0.20
GATED_VARIANT = "this model with Hypatia's same-index grid"
# Hypatia's ground-link rule (main_starlink_550.py): straight-line distance from a ground
# station to a satellite at most sqrt(940.7 km^2 + 550 km^2).
HYPATIA_MAX_GSL_KM = math.hypot(940.7, 550.0)

PLANE_STEP_DEG = 360.0 / ideal.NUM_ORBS
SLOT_STEP_DEG = 360.0 / ideal.NUM_SATS_PER_ORB
SATELLITES = ideal.NUM_ORBS * ideal.NUM_SATS_PER_ORB


# ---------------------------------------------------------------------------------------
# The shell by construction.
# ---------------------------------------------------------------------------------------
def index(orbit: int, slot: int) -> int:
    return orbit * ideal.NUM_SATS_PER_ORB + slot


def phase_deg(orbit: int) -> float:
    return ideal.Slot(orbit, 0).mean_anomaly_deg


def east_partner(orbit: int, slot: int, rule: str) -> tuple[int, int]:
    east = (orbit + 1) % ideal.NUM_ORBS
    if rule == "hypatia":
        return east, slot
    shift_deg = PLANE_STEP_DEG * math.cos(math.radians(ideal.INCLINATION_DEG))
    target_deg = slot * SLOT_STEP_DEG + phase_deg(orbit) - shift_deg - phase_deg(east)
    return east, round(target_deg / SLOT_STEP_DEG) % ideal.NUM_SATS_PER_ORB


def grid_links(rule: str) -> list[tuple[int, int]]:
    """Every +Grid link as (a, b) satellite indices, a < b."""
    links = set()
    for o in range(ideal.NUM_ORBS):
        for s in range(ideal.NUM_SATS_PER_ORB):
            ahead = index(o, (s + 1) % ideal.NUM_SATS_PER_ORB)
            links.add(tuple(sorted((index(o, s), ahead))))
            links.add(tuple(sorted((index(o, s), index(*east_partner(o, s, rule))))))
    return sorted(links)


def satrecs() -> SatrecArray:
    deg = math.pi / 180.0
    epoch = st.parse_utc(ideal.EPOCH)
    records = []
    for x in ideal.all_slots():
        satrec = Satrec()
        satrec.sgp4init(
            WGS72,
            "i",
            1,  # python-sgp4 refuses six-digit numbers; SGP4 never uses it.
            (epoch.jd_whole - SGP4_EPOCH_JD) + epoch.jd_fraction,
            0.0,
            0.0,
            0.0,
            ideal.ECCENTRICITY,
            ideal.ARG_OF_PERIGEE_DEG * deg,
            ideal.INCLINATION_DEG * deg,
            x.mean_anomaly_deg * deg,
            ideal.MEAN_MOTION_REV_PER_DAY / 720.0 * math.pi,
            x.raan_deg * deg,
        )
        satrec.jdsatepoch = epoch.jd_whole
        satrec.jdsatepochF = epoch.jd_fraction
        records.append(satrec)
    return SatrecArray(records)


def positions_itrs_km(ts, times: list[st.UtcInstant]) -> np.ndarray:
    """ITRS positions, shape (times, satellites, 3), in construction order."""
    jd = np.array([t.jd_whole for t in times])
    fr = np.array([t.jd_fraction for t in times])
    errors, teme_km, _ = satrecs().sgp4(jd, fr)
    if np.any(errors):
        raise SystemExit("error: SGP4 failed on the ideal shell")
    t = ts.utc(
        [x.year for x in times],
        [x.month for x in times],
        [x.day for x in times],
        [x.hour for x in times],
        [x.minute for x in times],
        [x.second for x in times],
    )
    # r_itrs = R_itrs(t) R_teme(t)^T r_teme: both rotations are from GCRS, Skyfield's own.
    teme_from_gcrs = TEME.rotation_at(t)
    itrs_from_gcrs = itrs.rotation_at(t)
    if teme_from_gcrs.ndim == 2:
        teme_from_gcrs = teme_from_gcrs[:, :, None]
        itrs_from_gcrs = itrs_from_gcrs[:, :, None]
    itrs_from_teme = np.einsum("ijn,kjn->nik", itrs_from_gcrs, teme_from_gcrs)
    return np.einsum("nik,snk->nsi", itrs_from_teme, teme_km)


def clears_atmosphere(a: np.ndarray, b: np.ndarray) -> bool:
    """The segment a-b stays at least 80 km above the equatorial-radius sphere."""
    d = b - a
    t = min(1.0, max(0.0, -float(a @ d) / float(d @ d)))
    return float(np.linalg.norm(a + t * d)) >= EQUATORIAL_RADIUS_KM + ATMOSPHERE_KM


# ---------------------------------------------------------------------------------------
# Cities and paths.
# ---------------------------------------------------------------------------------------
@dataclass
class City:
    name: str
    position_km: np.ndarray
    up: np.ndarray


def load_cities(path: Path) -> tuple[dict[str, City], list[tuple[str, str, str]]]:
    with path.open(encoding="utf-8") as f:
        data = json.load(f)
    cities = {}
    for c in data["cities"]:
        lat, lon = math.radians(c["lat"]), math.radians(c["lon"])
        position = wgs84.latlon(c["lat"], c["lon"], elevation_m=0.0).itrs_xyz.km
        up = np.array([math.cos(lat) * math.cos(lon), math.cos(lat) * math.sin(lon), math.sin(lat)])
        cities[c["name"]] = City(c["name"], np.asarray(position), up)
    pairs = [(p["from"], p["to"], p["role"]) for p in data["pairs"]]
    return cities, pairs


def ground_links(city: City, sats_km: np.ndarray, rule: str) -> dict[int, float]:
    """Satellites the city may use, with the link length in km."""
    d = sats_km - city.position_km
    length = np.linalg.norm(d, axis=1)
    if rule == "hypatia-gsl":
        ok = length <= HYPATIA_MAX_GSL_KM
    else:
        elevation = np.degrees(np.arcsin((d @ city.up) / length))
        ok = elevation >= MASK_DEG
    return {int(i): float(length[i]) for i in np.flatnonzero(ok)}


@dataclass
class PathResult:
    status: str
    one_way_ms: float | None = None
    hops: int | None = None
    first: int | None = None
    last: int | None = None


def shortest_from(
    up: dict[int, float], adjacency: list[list[tuple[int, float]]]
) -> tuple[list[float], list[int], list[int]]:
    """Dijkstra from a city over the satellites: distance, laser hops and first satellite."""
    dist = [math.inf] * SATELLITES
    hops = [0] * SATELLITES
    first = [-1] * SATELLITES
    heap = []
    for s, km in up.items():
        dist[s], first[s] = km, s
        heap.append((km, s))
    heapq.heapify(heap)
    done = [False] * SATELLITES
    while heap:
        d, s = heapq.heappop(heap)
        if done[s]:
            continue
        done[s] = True
        for t, km in adjacency[s]:
            if d + km < dist[t]:
                dist[t], hops[t], first[t] = d + km, hops[s] + 1, first[s]
                heapq.heappush(heap, (dist[t], t))
    return dist, hops, first


def best_case(up: dict[int, float], down: dict[int, float], tree) -> PathResult:
    if not up or not down:
        return PathResult("no_coverage")
    dist, hops, first = tree
    best = min(down, key=lambda s: (dist[s] + down[s], s))
    total = dist[best] + down[best]
    if math.isinf(total):
        return PathResult("no_path")
    return PathResult("ok", total / C_KM_S * 1e3, hops[best], first[best], best)


@dataclass
class Model:
    """The independent best case at every time, for one grid rule and one ground rule."""

    grid: str
    ground: str
    links_down: int = 0
    results: dict[tuple[int, str, str], PathResult] = field(default_factory=dict)


def run_model(
    model: Model,
    positions: np.ndarray,
    cities: dict[str, City],
    directions: list[tuple[str, str]],
) -> Model:
    links = grid_links(model.grid)
    sources = sorted({a for a, _ in directions})
    for k in range(positions.shape[0]):
        sats = positions[k]
        adjacency: list[list[tuple[int, float]]] = [[] for _ in range(SATELLITES)]
        for a, b in links:
            if not clears_atmosphere(sats[a], sats[b]):
                model.links_down += 1
                continue
            km = float(np.linalg.norm(sats[a] - sats[b]))
            adjacency[a].append((b, km))
            adjacency[b].append((a, km))
        visible = {name: ground_links(cities[name], sats, model.ground) for name in cities}
        for source in sources:
            tree = shortest_from(visible[source], adjacency)
            for a, b in directions:
                if a == source:
                    model.results[(k, a, b)] = best_case(visible[a], visible[b], tree)
    return model


def greedy_nearest_matching(sats: np.ndarray) -> set[tuple[int, int]]:
    """Cross-plane links by the program's documented rule (spec 4.3), written again: between
    planes o and o + 1, candidate pairs shortest first, accepted while both are free."""
    links = set()
    n = ideal.NUM_SATS_PER_ORB
    for o in range(ideal.NUM_ORBS):
        east = (o + 1) % ideal.NUM_ORBS
        candidates = sorted(
            (float(np.linalg.norm(sats[index(o, s)] - sats[index(east, t)])), s, t)
            for s in range(n)
            for t in range(n)
        )
        west_free, east_free = set(range(n)), set(range(n))
        for _, s, t in candidates:
            if s in west_free and t in east_free:
                west_free.discard(s)
                east_free.discard(t)
                links.add(tuple(sorted((index(o, s), index(east, t)))))
    return links


# ---------------------------------------------------------------------------------------
# The programs.
# ---------------------------------------------------------------------------------------
def run(command: list[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(
            f"error: {Path(command[0]).name} exited with {result.returncode}\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


def check_constellation(exe: Path, snapshot: Path, work: Path) -> list[str]:
    out = work / "constellation.csv"
    run(
        [
            str(exe),
            "--snapshot",
            str(snapshot),
            "--time",
            MAIN_START,
            "--shells-file",
            str(snapshot / ideal.SHELLS_FILE),
            "--out",
            str(out),
        ]
    )
    problems = []
    with out.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    if len(rows) != SATELLITES:
        problems.append(f"constellation: {len(rows)} rows, expected {SATELLITES}")
    planes = set()
    for row in rows:
        x = ideal.slot_of(int(row["norad_cat_id"]))
        source = "standard" if x.orbit == 0 else "supplemental"
        got = (row["shell"], row["plane_index"], row["index_in_plane"], row["element_source"])
        expected = (ideal.SHELL_NAME, str(x.orbit), str(x.slot), source)
        if got != expected:
            problems.append(f"constellation: {row['norad_cat_id']} is {got}, expected {expected}")
        planes.add(row["plane_index"])
    if len(planes) != ideal.NUM_ORBS:
        problems.append(f"constellation: {len(planes)} planes, expected {ideal.NUM_ORBS}")
    heights = [float(r["mean_height_km"]) for r in rows]
    print(
        f"constellation: {len(rows)} satellites, {len(planes)} planes, every plane and index as "
        f"constructed; mean height {min(heights):.2f} to {max(heights):.2f} km (WGS-84)"
    )
    return problems[:20]


def run_topology(
    exe: Path, snapshot: Path, cities: Path, ground: Path, duration_s: int, step_s: int, out: Path
) -> tuple[str, list[dict]]:
    summary = run(
        [
            str(exe),
            "--snapshot",
            str(snapshot),
            "--ground",
            str(ground),
            "--cities",
            str(cities),
            "--start",
            MAIN_START,
            "--duration-s",
            str(duration_s),
            "--step-s",
            str(step_s),
            "--pattern",
            "plus-grid",
            "--model",
            "best-case",
            "--shells-file",
            str(snapshot / ideal.SHELLS_FILE),
            "--out",
            str(out),
        ]
    )
    with out.open(newline="", encoding="utf-8") as f:
        return summary, list(csv.DictReader(f))


def check_summary(summary: str, steps: int) -> list[str]:
    expected_lines = [
        f"shell {ideal.SHELL_NAME}: {SATELLITES} satellites in {ideal.NUM_ORBS} planes",
        "satellites excluded: 0",
        f"planned laser links: {2 * SATELLITES}",
        f"in_plane: {SATELLITES}",
        f"cross_plane: {SATELLITES}",
        "second_cross_plane: 0",
        "lasers without a link: plane_too_small 0 no_neighbour_plane 0 no_free_partner 0",
        f"link-steps up: {2 * SATELLITES * steps}, down: end_not_propagated 0 "
        "no_line_of_sight 0 over_range_cap 0 sun_exclusion 0 locking_on 0",
        "checks: PASSED",
    ]
    lines = {line.strip() for line in summary.splitlines()}
    return [f"topology summary lacks {e!r}" for e in expected_lines if e not in lines]


def compare_rows(
    rows: list[dict], model: Model, times: list[st.UtcInstant]
) -> tuple[list[str], list[float], dict]:
    """Program rows against a model: problems, and every one-way delay difference."""
    by_time = {t.unix_ns: k for k, t in enumerate(times)}
    problems, differences = [], []
    per_direction: dict[tuple[str, str], list[float]] = {}
    seen = set()
    for row in rows:
        k = by_time.get(st.parse_utc(row["utc"]).unix_ns)
        key = (k, row["from"], row["to"])
        expected = model.results.get(key)
        if k is None or expected is None:
            problems.append(f"unexpected row {row['utc']} {row['from']} -> {row['to']}")
            continue
        seen.add(key)
        where = f"{row['utc']} {row['from']} -> {row['to']}"
        if row["status"] != expected.status:
            problems.append(f"{where}: status {row['status']}, model {expected.status}")
            continue
        if expected.status != "ok":
            continue
        diff = abs(float(row["one_way_ms"]) - expected.one_way_ms)
        differences.append(diff)
        per_direction.setdefault((row["from"], row["to"]), []).append(diff)
        got = (
            int(row["laser_hops"]),
            int(row["first_norad_cat_id"]),
            int(row["last_norad_cat_id"]),
        )
        want = (
            expected.hops,
            ideal.FIRST_NORAD_CAT_ID + expected.first,
            ideal.FIRST_NORAD_CAT_ID + expected.last,
        )
        if got != want:
            problems.append(f"{where}: hops, first, last {got}, model {want}")
        if diff > TOLERANCE_MS:
            problems.append(
                f"{where}: one-way {float(row['one_way_ms']):.9f} ms, model "
                f"{expected.one_way_ms:.9f} ms (difference {diff:.3e} ms)"
            )
    missing = set(model.results) - seen
    if missing:
        problems.append(f"{len(missing)} model rows have no program row")
    return problems, differences, per_direction


def model_differences(a: Model, b: Model) -> dict[tuple[str, str], list[float]]:
    """b minus a, one-way ms, per direction, where both are ok."""
    out: dict[tuple[str, str], list[float]] = {}
    for key, ra in a.results.items():
        rb = b.results.get(key)
        if ra.status == "ok" and rb is not None and rb.status == "ok":
            out.setdefault(key[1:], []).append(rb.one_way_ms - ra.one_way_ms)
    return out


def round_trips(rows: list[dict], a: str, b: str) -> list[float]:
    return [
        float(r["round_trip_ms"])
        for r in rows
        if r["from"] == a and r["to"] == b and r["status"] == "ok"
    ]


def model_round_trips(model: Model, a: str, b: str) -> list[float]:
    return [
        2.0 * r.one_way_ms
        for (_, x, y), r in model.results.items()
        if (x, y) == (a, b) and r.status == "ok"
    ]


# ---------------------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    st.forbid_network()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--topology", type=Path, required=True)
    parser.add_argument("--constellation", type=Path, required=True)
    parser.add_argument("--cities", type=Path, default=st.REPO_ROOT / "data/ground/cities.json")
    parser.add_argument("--ground", type=Path, default=st.REPO_ROOT / "data/ground/2026-11-19")
    parser.add_argument(
        "--summary-out",
        type=Path,
        help="also write the per-direction agreement as CSV (docs/phase-2-data/crosscheck.csv)",
    )
    args = parser.parse_args(argv)
    for exe in (args.topology, args.constellation):
        if not exe.is_file():
            print(f"error: {exe} not found", file=sys.stderr)
            return 2

    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="satsim-validate-topology-") as work_dir:
        work = Path(work_dir)
        snapshot = work / "ideal-shell"
        ideal.write_ideal_shell(snapshot, st.DEFAULT_SNAPSHOT / st.EOP_FILE)
        problems += check_constellation(args.constellation, snapshot, work)

        ts = skyfield_timescale(snapshot)
        cities, pairs = load_cities(args.cities)
        directions = [(a, b) for a, b, _ in pairs] + [(b, a) for a, b, _ in pairs]
        start = st.parse_utc(MAIN_START)

        # 1. The main cross-check.
        steps = MAIN_DURATION_S // MAIN_STEP_S + 1
        times = [start.plus_ns(k * MAIN_STEP_S * st.NS_PER_S) for k in range(steps)]
        summary, rows = run_topology(
            args.topology,
            snapshot,
            args.cities,
            args.ground,
            MAIN_DURATION_S,
            MAIN_STEP_S,
            work / "main.csv",
        )
        problems += check_summary(summary, steps)
        positions = positions_itrs_km(ts, times)
        nearest = run_model(Model("nearest", "mask"), positions, cities, directions)
        hypatia = run_model(Model("hypatia", "mask"), positions, cities, directions)
        if nearest.links_down or hypatia.links_down:
            problems.append(f"model links without line of sight: {nearest.links_down}")
        row_problems, differences, per_direction = compare_rows(rows, nearest, times)
        problems += row_problems

        statuses: dict[str, int] = {}
        for r in rows:
            statuses[r["status"]] = statuses.get(r["status"], 0) + 1
        print(
            f"\ntopology plus-grid best-case: {len(rows)} rows ({len(directions)} directions x "
            f"{steps} times every {MAIN_STEP_S} s); statuses {statuses}"
        )
        if differences:
            print(
                f"one-way delay against the independent model (nearest-slot grid): median "
                f"{float(np.median(differences)):.3e} ms, worst {max(differences):.3e} ms, "
                f"tolerance {TOLERANCE_MS:.0e} ms"
            )

        # Why the grids differ: the program's rule written again, against both constructions.
        greedy = greedy_nearest_matching(positions[0])
        for rule in ("nearest", "hypatia"):
            cross = {
                tuple(sorted((index(o, s), index(*east_partner(o, s, rule)))))
                for o in range(ideal.NUM_ORBS)
                for s in range(ideal.NUM_SATS_PER_ORB)
            }
            print(
                f"cross-plane links: greedy nearest-distance matching and the {rule!r} grid "
                f"share {len(greedy & cross)} of {len(cross)}"
            )
        lengths = {"same index (Hypatia)": [], "nearest slot": []}
        for o in range(ideal.NUM_ORBS):
            for s in range(ideal.NUM_SATS_PER_ORB):
                for label, rule in (
                    ("same index (Hypatia)", "hypatia"),
                    ("nearest slot", "nearest"),
                ):
                    other = index(*east_partner(o, s, rule))
                    lengths[label].append(
                        float(np.linalg.norm(positions[0][index(o, s)] - positions[0][other]))
                    )
        for label, values in lengths.items():
            print(
                f"  cross-plane link length at the start, {label}: {min(values):.0f} to "
                f"{max(values):.0f} km, mean {float(np.mean(values)):.0f} km"
            )
        grid_effect = model_differences(nearest, hypatia)
        all_effect = [v for values in grid_effect.values() for v in values]
        print(
            "Hypatia's same-index grid minus the nearest-slot grid, one-way: "
            f"{sum(1 for v in all_effect if abs(v) > TOLERANCE_MS)} of {len(all_effect)} rows "
            f"differ; median {float(np.median(all_effect)):.3f} ms, largest "
            f"{max(all_effect):.3f} ms, smallest {min(all_effect):.3f} ms"
        )

        # 2. Hypatia's published values.
        print("\nHypatia's published round-trip times (200 s from the epoch):")
        steps_h = HYPATIA_DURATION_S // HYPATIA_STEP_S + 1
        times_h = [start.plus_ns(k * HYPATIA_STEP_S * st.NS_PER_S) for k in range(steps_h)]
        _, rows_h = run_topology(
            args.topology,
            snapshot,
            args.cities,
            args.ground,
            HYPATIA_DURATION_S,
            HYPATIA_STEP_S,
            work / "hypatia.csv",
        )
        positions_h = positions_itrs_km(ts, times_h)
        published_pairs = [pair for pair, _, _ in HYPATIA_PUBLISHED]
        directions_h = published_pairs + [(b, a) for a, b in published_pairs]
        variants = {
            GATED_VARIANT: Model("hypatia", "mask"),
            "Hypatia's grid and ground-link rule": Model("hypatia", "hypatia-gsl"),
        }
        for model in variants.values():
            run_model(model, positions_h, cities, directions_h)
        for (a, b), (low, high), source in HYPATIA_PUBLISHED:
            rtt = round_trips(rows_h, a, b)
            if len(rtt) != steps_h:
                problems.append(f"{a} -> {b}: {len(rtt)} ok steps of {steps_h}")
                continue
            got = (min(rtt), max(rtt))
            print(f"  {a} -> {b}: published {low:.0f} to {high:.0f} ms ({source})")
            print(f"    program (plus-grid, best case): {got[0]:.1f} to {got[1]:.1f} ms")
            for name, model in variants.items():
                values = model_round_trips(model, a, b)
                print(f"    {name}: {min(values):.1f} to {max(values):.1f} ms")
            # Gating: this model with Hypatia's own grid must give Hypatia's numbers, which
            # checks the shell, the times, the cities and the 25 deg mask against Hypatia. The
            # program's +Grid is a different link set on this shell (see above), so its values
            # are reported, not held to Hypatia's.
            same_grid = model_round_trips(variants[GATED_VARIANT], a, b)
            for label, published, model_value, program_value in (
                ("shortest", low, min(same_grid), got[0]),
                ("longest", high, max(same_grid), got[1]),
            ):
                relative = (model_value - published) / published
                program_relative = (program_value - published) / published
                print(
                    f"    {label}: model with Hypatia's grid {relative:+.1%}, program "
                    f"{program_relative:+.1%} from the published value"
                )
                if abs(relative) > HYPATIA_TOLERANCE:
                    problems.append(
                        f"{a} -> {b}: {label} round trip with Hypatia's grid "
                        f"{model_value:.1f} ms is {relative:+.1%} from Hypatia's "
                        f"{published:.0f} ms (limit 20%)"
                    )

        if args.summary_out:
            write_summary(args.summary_out, rows, per_direction, grid_effect, rows_h, variants)

    if problems:
        print(f"\nFAIL: {len(problems)} problems", file=sys.stderr)
        for line in problems[:40]:
            print(f"  {line}", file=sys.stderr)
        return 1
    print(
        "\nPASS: counts, plane assignment and every row agree; with Hypatia's grid, Hypatia's "
        "values within 20%"
    )
    return 0


def write_summary(path, rows, per_direction, grid_effect, rows_h, variants) -> None:
    """Small committed CSV for the report and its plot."""
    path.parent.mkdir(parents=True, exist_ok=True)

    def stats(values):
        return [f"{min(values):.4f}", f"{float(np.median(values)):.4f}", f"{max(values):.4f}"]

    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(
            [
                "kind",
                "from",
                "to",
                "rows",
                "worst_abs_difference_ms",
                "min_ms",
                "median_ms",
                "max_ms",
            ]
        )
        for (a, b), values in sorted(per_direction.items()):
            w.writerow(
                ["program_minus_model_one_way", a, b, len(values), f"{max(values):.3e}", "", "", ""]
            )
        for (a, b), values in sorted(grid_effect.items()):
            w.writerow(
                ["hypatia_grid_minus_nearest_grid_one_way", a, b, len(values), "", *stats(values)]
            )
        for (a, b), _, _ in HYPATIA_PUBLISHED:
            for x, y in ((a, b), (b, a)):
                rtt = round_trips(rows_h, x, y)
                w.writerow(["program_round_trip_200s", x, y, len(rtt), "", *stats(rtt)])
                for name, model in variants.items():
                    values = model_round_trips(model, x, y)
                    w.writerow(
                        [f"model_round_trip_200s ({name})", x, y, len(values), "", *stats(values)]
                    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Cross-check the routing program against an independent model (docs/phase-3-routing.md 6.3).

Writes an ideal published shell with its link file (python/make_ideal_shell.py), runs the C++
`routing` program on it, and recomputes every row here from the spec, sharing no code with
the program:

  positions   python-sgp4 from the elements BY CONSTRUCTION (plane and slot -> elements), not
              read back from the files. TEME to ITRF written here from the frame the spec
              documents (Vallado's convention, phase-1-orbits.md): GMST (IAU 1982) at
              UT1 = UTC + (UT1 - UTC), then polar motion r_ITRF = ROT2(-x_p) ROT1(-y_p) r_PEF,
              with UT1 - UTC, x_p and y_p read from the same finals2000A.all here and
              interpolated linearly in UTC between its daily rows. Checked against Skyfield's
              own TEME and ITRS frames to POSITION_CHECK_M (Skyfield adds the TIO locator s',
              about 0 at the shell's epoch, 2000-01-01). Times come from integer nanoseconds,
              never one float Julian date.
  lasers      the link file's pairs; a link is up when the segment between its ends stays
              80 km above a sphere of radius 6378.137 km (Phase 2's line of sight; no range
              cap, no Sun exclusion, no lock-on time in these runs).
  ground      a city at height 0 on the WGS-84 ellipsoid may use a satellite within
              --max-ground-range-km (no elevation test), or at or above --min-elevation-deg
              measured from the ellipsoid normal. Ground and laser edges carry length / c plus
              --hop-delay-ms.
  graph       per step, best case (spec 4.2): sending dish -> every usable satellite, laser
              edges both ways, every usable satellite -> receiving dish. Satellites are nodes
              0 to N - 1 in catalogue order.
  paths       Dijkstra written here with the spec's tie rule (4.5): lowest delay (the sum
              formed predecessor's delay + edge delay, compared exactly), then fewest hops,
              then the lowest-numbered predecessor.
  strategies  the re-plan loop of 4.3 and 4.5 for lowest-delay, keep-until-broken and
              longest-lived (best case: the north and south candidate of each city by time
              left in view, 4.4, then elevation, then catalogue number; the lowest-delay pair
              that lasers join, north head first, then north tail; the shortest path through
              it), the `start` planner; drops and their causes (4.6); the change counts of
              4.7.

Agreement required (exit 1 otherwise), every row of every case: status, drop cause, the
route (every satellite, in order), laser hops, first and last satellite, plan, one-way and
ideal delay within TOLERANCE_MS; and per direction and strategy the summary's counts of bad
changes, delay increases, breaks and recoveries.

Cases (all on committed inputs; CASES below):
  hypatia      Hypatia's shell and link file, --max-ground-range-km 1089.686, the cities of
               data/ground/cities.json both ways, 300 s at 1 s, re-planned every step.
  bariloche    the Bariloche shell and link file, --min-elevation-deg 40, --hop-delay-ms
               1.012, the same cities plus Bariloche -> Beijing, 600 s at 1 s, re-planned
               every 5 s (routes held between plans, so held routes break and drop).
  hypatia-0.1s Hypatia's setup as reproduced (6.3): 100 ms steps, lowest-delay every step,
               --directions forward, Hypatia's first 10 cities (45 pairs), 10 s.

Usage (inside the CI image):

    python3 python/validate_routing.py --routing build/validate/bin/routing

Reads only local files; any network access raises.
"""

from __future__ import annotations

import argparse
import csv
import heapq
import itertools
import json
import math
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from sgp4.api import WGS72, Satrec, SatrecArray
from skyfield.framelib import itrs
from skyfield.sgp4lib import TEME

import make_ideal_shell as ideal
import snapshot_tools as st
from validate_against_reference import skyfield_timescale

C_KM_S = 299_792.458
EQUATORIAL_RADIUS_KM = 6378.137
ATMOSPHERE_KM = 80.0
WGS84_A_KM = 6378.137
WGS84_F = 1.0 / 298.257223563
SGP4_EPOCH_JD = 2433281.5  # 1949 December 31 0h, the zero of sgp4init's epoch argument.
ARCSEC_RAD = math.pi / (180.0 * 3600.0)
MJD_UNIX_EPOCH = 40587
VIEW_HORIZON_NS = 900 * st.NS_PER_S  # Spec 4.4.

# One-way and ideal delay tolerance per row. Both sides compute the same model in double
# precision; what differs is rounding (the order of sums, the frame rotation). 1e-9 ms is
# 0.3 mm of path. A different link, satellite or ground link changes a delay by 0.01 ms or
# more.
TOLERANCE_MS = 1e-9
# This model's ITRF positions against Skyfield's: the same model reached by another route.
# Phase 1 measured 0.43 mm in 2026, all of it Skyfield's TIO locator, which is zero at J2000.
POSITION_CHECK_M = 1e-3

HYPATIA_RANGE_KM = 1089.686  # sqrt(940.7^2 + 550^2) km, main_starlink_550.py.
BARILOCHE = ("Bariloche", -41.13, -71.31)
BEIJING = ("Beijing", 39.90, 116.41)
STRATEGIES = ("lowest-delay", "keep-until-broken", "longest-lived")


@dataclass(frozen=True)
class Case:
    name: str
    setup: ideal.Setup
    duration_ns: int
    step_ns: int
    replan_ns: int
    strategies: tuple[str, ...]
    max_range_km: float | None = None
    min_elevation_deg: float | None = None
    hop_delay_ms: float = 0.0
    forward_only: bool = False
    start: str = "2000-01-01T00:00:00"

    def program_options(self) -> list[str]:
        options = [
            "--start",
            self.start,
            "--duration-s",
            st.seconds_text(self.duration_ns),
            "--step-s",
            st.seconds_text(self.step_ns),
            "--replan-s",
            st.seconds_text(self.replan_ns),
            "--strategies",
            ",".join(self.strategies),
            "--hop-delay-ms",
            repr(self.hop_delay_ms),
            "--directions",
            "forward" if self.forward_only else "both",
        ]
        if self.max_range_km is not None:
            options += ["--max-ground-range-km", repr(self.max_range_km)]
        if self.min_elevation_deg is not None:
            options += ["--min-elevation-deg", repr(self.min_elevation_deg)]
        return options


S = st.NS_PER_S
CASES = [
    Case("hypatia", ideal.HYPATIA, 300 * S, S, S, STRATEGIES, max_range_km=HYPATIA_RANGE_KM),
    Case(
        "bariloche",
        ideal.BARILOCHE,
        600 * S,
        S,
        5 * S,
        STRATEGIES,
        min_elevation_deg=40.0,
        hop_delay_ms=1.012,
    ),
    Case(
        "hypatia-0.1s",
        ideal.HYPATIA,
        10 * S,
        S // 10,
        S // 10,
        ("lowest-delay",),
        max_range_km=HYPATIA_RANGE_KM,
        forward_only=True,
    ),
]
# By hand only (about a minute): the whole Bariloche reproduction run of 6.3, which
# python/reproduce_routing.py bariloche makes at offset 0.
BY_HAND = [
    Case(
        "bariloche-orbit",
        ideal.BARILOCHE,
        5739 * S,
        S,
        S,
        STRATEGIES,
        min_elevation_deg=40.0,
        hop_delay_ms=1.012,
        forward_only=True,
    ),
]


# ---------------------------------------------------------------------------------------
# Positions, by construction.
# ---------------------------------------------------------------------------------------
def satrecs(setup: ideal.Setup) -> SatrecArray:
    deg = math.pi / 180.0
    epoch = st.parse_utc(ideal.EPOCH)
    records = []
    for x in ideal.all_slots(setup):
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
            setup.mean_motion_rev_per_day / 720.0 * math.pi,
            x.raan_deg * deg,
        )
        satrec.jdsatepoch = epoch.jd_whole
        satrec.jdsatepochF = epoch.jd_fraction
        records.append(satrec)
    return SatrecArray(records)


@dataclass(frozen=True)
class EarthOrientation:
    first_mjd: int
    ut1_minus_utc_s: list[float]
    x_rad: list[float]
    y_rad: list[float]

    def at(self, t: st.UtcInstant) -> tuple[float, float, float]:
        """UT1 - UTC, x_p, y_p, linear in UTC between the rows at 0h UTC."""
        day, ns = divmod(t.unix_ns, st.NS_PER_DAY)
        i = day + MJD_UNIX_EPOCH - self.first_mjd
        if not 0 <= i < len(self.x_rad) - 1:
            raise SystemExit(f"error: {t.iso()} is outside the IERS file")
        f = ns / st.NS_PER_DAY
        values = (self.ut1_minus_utc_s, self.x_rad, self.y_rad)
        return tuple(v[i] + f * (v[i + 1] - v[i]) for v in values)  # type: ignore[return-value]


def load_earth_orientation(path: Path) -> EarthOrientation:
    """finals2000A.all: MJD in columns 8-15, Bulletin A x_p 19-27 and y_p 38-46 (arcseconds),
    UT1 - UTC 59-68 (seconds); up to the first row missing one of them."""
    mjds, dut, xs, ys = [], [], [], []
    with path.open(encoding="ascii") as f:
        for line in f:
            fields = (line[7:15], line[18:27], line[37:46], line[58:68])
            if any(not x.strip() for x in fields):
                break
            mjds.append(round(float(fields[0])))
            xs.append(float(fields[1]) * ARCSEC_RAD)
            ys.append(float(fields[2]) * ARCSEC_RAD)
            dut.append(float(fields[3]))
    if any(b - a != 1 for a, b in itertools.pairwise(mjds)):
        raise SystemExit(f"error: {path} rows are not consecutive days")
    return EarthOrientation(mjds[0], dut, xs, ys)


def gmst82_rad(t: st.UtcInstant, ut1_minus_utc_s: float) -> float:
    """GMST, IAU 1982 (Vallado's gstime), at UT1 given as a two-part Julian date."""
    t_cent = ((t.jd_whole - 2451545.0) + (t.jd_fraction + ut1_minus_utc_s / 86400.0)) / 36525.0
    seconds = (
        67310.54841
        + (876600.0 * 3600.0 + 8640184.812866) * t_cent
        + 0.093104 * t_cent * t_cent
        - 6.2e-6 * t_cent * t_cent * t_cent
    )
    return math.fmod(seconds * (math.pi / 43200.0), 2.0 * math.pi) % (2.0 * math.pi)


def positions_itrf_km(
    sats: SatrecArray, eop: EarthOrientation, times: list[st.UtcInstant]
) -> np.ndarray:
    """ITRF positions, shape (times, satellites, 3), satellites in construction order."""
    jd = np.array([t.jd_whole for t in times])
    fr = np.array([t.jd_fraction for t in times])
    errors, teme_km, _ = sats.sgp4(jd, fr)
    if np.any(errors):
        raise SystemExit("error: SGP4 failed on the ideal shell")
    out = np.empty((len(times), teme_km.shape[0], 3))
    for k, t in enumerate(times):
        dut1, xp, yp = eop.at(t)
        g = gmst82_rad(t, dut1)
        x, y, z = teme_km[:, k, 0], teme_km[:, k, 1], teme_km[:, k, 2]
        # r_PEF = ROT3(gmst) r_TEME.
        px = math.cos(g) * x + math.sin(g) * y
        py = -math.sin(g) * x + math.cos(g) * y
        pz = z
        # ROT1(-y_p), then ROT2(-x_p).
        qy = math.cos(yp) * py - math.sin(yp) * pz
        qz = math.sin(yp) * py + math.cos(yp) * pz
        out[k, :, 0] = math.cos(xp) * px + math.sin(xp) * qz
        out[k, :, 1] = qy
        out[k, :, 2] = -math.sin(xp) * px + math.cos(xp) * qz
    return out


def skyfield_difference_m(
    sats: SatrecArray, snapshot: Path, times: list[st.UtcInstant], ours: np.ndarray
) -> float:
    """Largest distance between these positions and Skyfield's TEME -> ITRS of the same SGP4
    output, metres."""
    ts = skyfield_timescale(snapshot)
    jd = np.array([t.jd_whole for t in times])
    fr = np.array([t.jd_fraction for t in times])
    _, teme_km, _ = sats.sgp4(jd, fr)
    t = ts.utc(
        [x.year for x in times],
        [x.month for x in times],
        [x.day for x in times],
        [x.hour for x in times],
        [x.minute for x in times],
        [x.second for x in times],
    )
    teme_from_gcrs = TEME.rotation_at(t)
    itrs_from_gcrs = itrs.rotation_at(t)
    if teme_from_gcrs.ndim == 2:
        teme_from_gcrs = teme_from_gcrs[:, :, None]
        itrs_from_gcrs = itrs_from_gcrs[:, :, None]
    itrs_from_teme = np.einsum("ijn,kjn->nik", itrs_from_gcrs, teme_from_gcrs)
    theirs = np.einsum("nik,snk->nsi", itrs_from_teme, teme_km)
    return float(np.max(np.linalg.norm(theirs - ours, axis=2))) * 1e3


# ---------------------------------------------------------------------------------------
# Cities.
# ---------------------------------------------------------------------------------------
@dataclass
class City:
    name: str
    lat: float
    lon: float
    position_km: np.ndarray = field(init=False)
    up: np.ndarray = field(init=False)

    def __post_init__(self) -> None:
        phi, lam = math.radians(self.lat), math.radians(self.lon)
        e2 = WGS84_F * (2.0 - WGS84_F)
        n = WGS84_A_KM / math.sqrt(1.0 - e2 * math.sin(phi) ** 2)
        self.position_km = np.array(
            [
                n * math.cos(phi) * math.cos(lam),
                n * math.cos(phi) * math.sin(lam),
                n * (1.0 - e2) * math.sin(phi),
            ]
        )
        self.up = np.array(
            [math.cos(phi) * math.cos(lam), math.cos(phi) * math.sin(lam), math.sin(phi)]
        )


def city_record(name: str, lat: float, lon: float) -> dict:
    """A cities.json record for a place with no home-PoP lookup (best case only)."""
    return {
        "name": name,
        "country": "ZZ",
        "region": "ZZ",
        "geofeed_city": name,
        "lat": lat,
        "lon": lon,
    }


def directions_of(data: dict, forward_only: bool) -> list[tuple[str, str]]:
    out = []
    for p in data["pairs"]:
        out.append((p["from"], p["to"]))
        if not forward_only:
            out.append((p["to"], p["from"]))
    return out


# ---------------------------------------------------------------------------------------
# The model.
# ---------------------------------------------------------------------------------------
@dataclass
class Step:
    """The graph of one step: laser adjacency and each city's usable satellites."""

    lasers: list[list[tuple[int, float]]]
    laser_up: set[tuple[int, int]]
    ground: dict[str, dict[int, float]]  # city -> satellite -> edge delay (s), both ways.
    elevation: dict[str, dict[int, float]]


@dataclass
class Tree:
    delay: list[float]
    hops: list[int]
    pred: list[int]


DISH = -1  # Predecessor of a first satellite: the sending dish (a node after every satellite).


def dijkstra(step: Step, starts: dict[int, float], n: int) -> Tree:
    """Shortest paths over the satellites from the sending dish, whose edges are `starts`
    (satellite -> delay). Tie rule: delay, then hops, then lowest predecessor (spec 4.5); the
    dish is numbered after every satellite."""
    inf = math.inf
    delay, hops, pred = [inf] * n, [0] * n, [n] * n
    heap = []
    for s, d in starts.items():
        delay[s], hops[s], pred[s] = d, 1, n
        heap.append((d, 1, s))
    heapq.heapify(heap)
    done = [False] * n
    while heap:
        d, h, u = heapq.heappop(heap)
        if done[u] or d != delay[u] or h != hops[u]:
            continue
        done[u] = True
        for v, w in step.lasers[u]:
            if done[v]:
                continue
            nd = d + w
            if (nd, h + 1, u) < (delay[v], hops[v], pred[v]):
                delay[v], hops[v], pred[v] = nd, h + 1, u
                heapq.heappush(heap, (nd, h + 1, v))
    return Tree(delay, hops, pred)


def path_from(tree: Tree, n: int, last: int) -> list[int]:
    path = [last]
    while tree.pred[path[-1]] != n:
        path.append(tree.pred[path[-1]])
    return path[::-1]


def best_exit(tree: Tree, down: dict[int, float], n: int) -> tuple[float, list[int]] | None:
    """The receiving dish's best predecessor by the tie rule, and the route's satellites."""
    best = None
    for s, w in down.items():
        if math.isinf(tree.delay[s]):
            continue
        key = (tree.delay[s] + w, tree.hops[s] + 1, s)
        if best is None or key < best:
            best = key
    if best is None:
        return None
    return best[0], path_from(tree, n, best[2])


def route_delay(step: Step, src: str, dst: str, route: list[int]) -> float | None:
    """The route's delay in this step's graph (edges summed in route order), or None."""
    up, down = step.ground[src], step.ground[dst]
    if route[0] not in up or route[-1] not in down:
        return None
    total = up[route[0]]
    for a, b in itertools.pairwise(route):
        w = next((w for v, w in step.lasers[a] if v == b), None)
        if w is None:
            return None
        total += w
    return total + down[route[-1]]


def drop_cause(step: Step, src: str, dst: str, route: list[int] | None, plan_cause: str) -> str:
    if not step.ground[src] or not step.ground[dst]:
        return "no_coverage"
    if route is None:
        return plan_cause
    if route[0] not in step.ground[src] or route[-1] not in step.ground[dst]:
        return "ground_link_lost"
    return "laser_link_down"


@dataclass
class Row:
    status: str
    cause: str = ""
    one_way_ms: float | None = None
    ideal_ms: float | None = None
    route: tuple[int, ...] = ()
    plan: str = ""


@dataclass
class Held:
    route: list[int] | None = None
    plan_cause: str = "no_path"


class Model:
    def __init__(self, case: Case, cities: dict[str, City], directions, positions, links):
        self.case = case
        self.cities = cities
        self.directions = directions
        self.positions = positions  # (steps incl. look-ahead, satellites, 3)
        self.links = links
        self.n = positions.shape[1]
        self.hop_s = case.hop_delay_ms / 1000.0
        self.steps: dict[int, Step] = {}
        self.view_cache: dict[tuple[int, str], dict[int, int]] = {}

    # Ground visibility at step k, for every satellite: usable mask, length and elevation.
    def visibility(self, k: int, city: City) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        d = self.positions[k] - city.position_km
        length = np.sqrt(d[:, 0] * d[:, 0] + d[:, 1] * d[:, 1] + d[:, 2] * d[:, 2])
        elevation = np.arcsin((d @ city.up) / length)
        if self.case.max_range_km is not None:
            ok = length <= self.case.max_range_km
        else:
            ok = elevation >= math.radians(self.case.min_elevation_deg)
        return ok, length, elevation

    def step(self, k: int) -> Step:
        if k in self.steps:
            return self.steps[k]
        sats = self.positions[k]
        lasers: list[list[tuple[int, float]]] = [[] for _ in range(self.n)]
        up = set()
        a, b = self.links[:, 0], self.links[:, 1]
        d = sats[b] - sats[a]
        length = np.sqrt(d[:, 0] * d[:, 0] + d[:, 1] * d[:, 1] + d[:, 2] * d[:, 2])
        # Closest approach of the segment to the Earth's centre.
        t = np.clip(-np.einsum("ij,ij->i", sats[a], d) / (length * length), 0.0, 1.0)
        closest = np.linalg.norm(sats[a] + t[:, None] * d, axis=1)
        clear = closest >= EQUATORIAL_RADIUS_KM + ATMOSPHERE_KM
        for i in range(len(a)):
            if not clear[i]:
                continue
            w = float(length[i]) / C_KM_S + self.hop_s
            x, y = int(a[i]), int(b[i])
            lasers[x].append((y, w))
            lasers[y].append((x, w))
            up.add((min(x, y), max(x, y)))
        ground, elevation = {}, {}
        for name, city in self.cities.items():
            ok, ln, el = self.visibility(k, city)
            idx = np.flatnonzero(ok)
            ground[name] = {int(s): float(ln[s]) / C_KM_S + self.hop_s for s in idx}
            elevation[name] = {int(s): float(el[s]) for s in idx}
        self.steps[k] = Step(lasers, up, ground, elevation)
        if k - 2 in self.steps:
            del self.steps[k - 2]
        return self.steps[k]

    def time_left_steps(self, k: int, name: str) -> dict[int, int]:
        """Per satellite usable at step k: the first j >= 0 at which it is not usable (spec 4.4),
        with j * step < 900 s looked at; a satellite usable at every one of them gets the
        horizon, in steps (900 s / step, which is exact for these steps)."""
        key = (k, name)
        if key in self.view_cache:
            return self.view_cache[key]
        horizon = VIEW_HORIZON_NS // self.case.step_ns
        city = self.cities[name]
        ok0, _, _ = self.visibility(k, city)
        alive = ok0.copy()
        left = np.where(ok0, horizon, 0)
        for j in range(1, horizon):
            ok, _, _ = self.visibility(k + j, city)
            ended = alive & ~ok
            left[ended] = j
            alive &= ok
            if not alive.any():
                break
        out = {int(s): int(left[s]) for s in np.flatnonzero(ok0)}
        self.view_cache[key] = out
        return out

    def candidates(self, k: int, name: str) -> list[int | None]:
        """[north, south] candidate of a city at step k (spec 4.5)."""
        step = self.step(k)
        left = self.time_left_steps(k, name)
        z_now, z_next = self.positions[k][:, 2], self.positions[k + 1][:, 2]
        best: list[tuple | None] = [None, None]
        for s in step.ground[name]:
            north = 0 if z_next[s] > z_now[s] else 1
            key = (left[s], step.elevation[name][s], -s)
            if best[north] is None or key > best[north]:
                best[north] = key
        return [None if b is None else -b[2] for b in best]

    def longest_lived(self, k: int, src: str, dst: str) -> tuple[float, list[int]] | None:
        step = self.step(k)
        heads = self.candidates(k, src)
        tails = self.candidates(k, dst)
        best = None
        for h in heads:
            if h is None:
                continue
            tree = dijkstra(step, {h: step.ground[src][h]}, self.n)
            for t in tails:
                if t is None or math.isinf(tree.delay[t]):
                    continue
                total = tree.delay[t] + step.ground[dst][t]
                if best is None or total < best[0]:
                    best = (total, path_from(tree, self.n, t))
        return best

    def run(self, steps: int) -> dict[tuple[int, str, str, str], Row]:
        rows = {}
        replan = self.case.replan_ns // self.case.step_ns
        held = {(a, b, s): Held() for a, b in self.directions for s in self.case.strategies}
        for k in range(steps):
            step = self.step(k)
            trees: dict[str, Tree] = {}

            def shortest(src: str, dst: str, step=step, trees=trees):
                if src not in trees:
                    trees[src] = dijkstra(step, step.ground[src], self.n)
                return best_exit(trees[src], step.ground[dst], self.n)

            for src, dst in self.directions:
                ideal_route = shortest(src, dst)
                for strategy in self.case.strategies:
                    h = held[(src, dst, strategy)]
                    plan = ""
                    if k % replan == 0:
                        old = h.route
                        covered = bool(step.ground[src]) and bool(step.ground[dst])
                        keep = (
                            strategy != "lowest-delay"
                            and old is not None
                            and route_delay(step, src, dst, old) is not None
                        )
                        if not keep:
                            if not covered:
                                found = None
                            elif strategy == "longest-lived":
                                found = self.longest_lived(k, src, dst)
                            else:
                                found = ideal_route
                            h.route = None if found is None else found[1]
                            h.plan_cause = "no_coverage" if not covered else "no_path"
                        if h.route is None:
                            plan = "none"
                        else:
                            plan = "kept" if h.route == old else "replaced"
                    delay = None if h.route is None else route_delay(step, src, dst, h.route)
                    row = Row("ok" if delay is not None else "drop", plan=plan)
                    if ideal_route is not None:
                        row.ideal_ms = ideal_route[0] * 1e3
                    if delay is None:
                        row.cause = drop_cause(step, src, dst, h.route, h.plan_cause)
                    else:
                        row.one_way_ms = delay * 1e3
                        row.route = tuple(h.route)  # type: ignore[arg-type]
                    rows[(k, src, dst, strategy)] = row
        return rows


# ---------------------------------------------------------------------------------------
# Change counts (spec 4.7) and Hypatia's rule (6.3).
# ---------------------------------------------------------------------------------------
@dataclass
class Counts:
    changes: int = 0
    bad: int = 0
    increases: int = 0
    breaks: int = 0
    recoveries: int = 0


def count_changes(series: list[Row]) -> Counts:
    c = Counts()
    for old, new in itertools.pairwise(series):
        if old.status == "ok" and new.status == "ok" and old.route != new.route:
            c.changes += 1
            if new.one_way_ms < old.one_way_ms:  # type: ignore[operator]
                c.bad += 1
            elif new.one_way_ms > old.one_way_ms:  # type: ignore[operator]
                c.increases += 1
        elif old.status == "ok" and new.status != "ok":
            c.breaks += 1
        elif old.status != "ok" and new.status == "ok":
            c.recoveries += 1
    return c


def hypatia_changes(routes: list[tuple[int, ...]]) -> int:
    """Hypatia's count: consecutive steps whose node lists differ, no route being the empty
    list (so losing the route is one change and regaining it another)."""
    return sum(1 for a, b in itertools.pairwise(routes) if a != b)


# ---------------------------------------------------------------------------------------
# The program.
# ---------------------------------------------------------------------------------------
def run_routing(exe: Path, snapshot: Path, ground: Path, cities: Path, case: Case, out: Path):
    command = [
        str(exe),
        "--snapshot",
        str(snapshot),
        "--ground",
        str(ground),
        "--cities",
        str(cities),
        "--pattern",
        "plus-grid",
        "--model",
        "best-case",
        "--shells-file",
        str(snapshot / ideal.SHELLS_FILE),
        "--links-file",
        str(snapshot / ideal.LINKS_FILE),
        "--out",
        str(out),
        *case.program_options(),
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(
            f"error: routing exited with {result.returncode}\n{result.stdout}{result.stderr}"
        )
    with out.open(newline="", encoding="utf-8") as f:
        return result.stdout, list(csv.DictReader(f))


_DIRECTION = re.compile(r"^    (.+) -> (.+) \((study|validation), ")
_STRATEGY = re.compile(r"^      ([a-z:%.0-9-]+): ok \d+ of \d+ steps")
_COUNTS = re.compile(r"counts: bad (\d+), increases (\d+), breaks (\d+), recoveries (\d+)$")


def summary_counts(summary: str) -> dict[tuple[str, str, str], tuple[int, int, int, int]]:
    out, direction, strategy = {}, None, None
    for line in summary.splitlines():
        if m := _DIRECTION.match(line):
            direction = (m.group(1), m.group(2))
        elif m := _STRATEGY.match(line):
            strategy = m.group(1)
        elif (m := _COUNTS.search(line)) and direction and strategy:
            out[(*direction, strategy)] = tuple(int(x) for x in m.groups())
    return out


def compare(case: Case, program_rows, model_rows, times, summary, sat_ids) -> list[str]:
    problems = []
    by_time = {t.unix_ns: k for k, t in enumerate(times)}
    seen = set()
    largest = 0.0
    for r in program_rows:
        k = by_time.get(st.parse_utc(r["utc"]).unix_ns)
        key = (k, r["from"], r["to"], r["strategy"])
        want = model_rows.get(key)
        where = f"{case.name} {r['utc']} {r['from']} -> {r['to']} {r['strategy']}"
        if want is None:
            problems.append(f"{where}: no model row")
            continue
        seen.add(key)
        route = tuple(int(x) for x in r["route"].split()) if r["route"] else ()
        expected_route = tuple(sat_ids[s] for s in want.route)
        got = (r["status"], r["drop_cause"], route, r["plan"])
        expected = (want.status, want.cause, expected_route, want.plan)
        if got != expected:
            problems.append(f"{where}: program {got}, model {expected}")
            continue
        if want.status == "ok":
            ends = (int(r["laser_hops"]), int(r["first_norad_cat_id"]), int(r["last_norad_cat_id"]))
            if ends != (len(route) - 1, route[0], route[-1]):
                problems.append(f"{where}: laser hops, first, last {ends} for route {route}")
            diff = abs(float(r["one_way_ms"]) - want.one_way_ms)  # type: ignore[operator]
            largest = max(largest, diff)
            if diff > TOLERANCE_MS:
                problems.append(f"{where}: one-way {r['one_way_ms']} ms, model {want.one_way_ms}")
        if (r["ideal_ms"] == "") != (want.ideal_ms is None):
            problems.append(f"{where}: ideal {r['ideal_ms']!r}, model {want.ideal_ms}")
        elif want.ideal_ms is not None and abs(float(r["ideal_ms"]) - want.ideal_ms) > TOLERANCE_MS:
            problems.append(f"{where}: ideal {r['ideal_ms']} ms, model {want.ideal_ms}")
    missing = set(model_rows) - seen
    if missing:
        problems.append(f"{case.name}: {len(missing)} model rows have no program row")
    print(
        f"  one-way delay, program against model: largest difference {largest:.2e} ms "
        f"(tolerance {TOLERANCE_MS:.0e} ms)"
    )

    reported = summary_counts(summary)
    for a, b, s in sorted({key[1:] for key in model_rows}):
        series = [model_rows[(k, a, b, s)] for k in range(len(times))]
        c = count_changes(series)
        got = reported.get((a, b, s))
        if got != (c.bad, c.increases, c.breaks, c.recoveries):
            problems.append(
                f"{case.name} {a} -> {b} {s}: summary counts {got}, model "
                f"{(c.bad, c.increases, c.breaks, c.recoveries)}"
            )
    return problems


def describe(case: Case, model_rows, times, program_rows) -> None:
    statuses: dict[str, int] = {}
    causes: dict[str, int] = {}
    plans: dict[str, int] = {}
    for r in program_rows:
        statuses[r["status"]] = statuses.get(r["status"], 0) + 1
        if r["drop_cause"]:
            causes[r["drop_cause"]] = causes.get(r["drop_cause"], 0) + 1
        if r["plan"]:
            plans[r["plan"]] = plans.get(r["plan"], 0) + 1
    print(f"  rows {len(program_rows)}: {statuses}; drop causes {causes}; plans {plans}")
    for s in case.strategies:
        totals = Counts()
        hyp = 0
        for a, b in sorted({(key[1], key[2]) for key in model_rows}):
            series = [model_rows[(k, a, b, s)] for k in range(len(times))]
            c = count_changes(series)
            for name in ("changes", "bad", "increases", "breaks", "recoveries"):
                setattr(totals, name, getattr(totals, name) + getattr(c, name))
            hyp += hypatia_changes([x.route for x in series])
        print(
            f"  {s}: changes {totals.changes} (bad {totals.bad}, increases {totals.increases}), "
            f"breaks {totals.breaks}, recoveries {totals.recoveries}; Hypatia's count {hyp}"
        )


# ---------------------------------------------------------------------------------------
def prepare_cities(case: Case, cities_file: Path, hypatia_cities: Path, work: Path):
    """The cities file each case runs on, written to `work`, and the model's cities."""
    if case.name == "hypatia-0.1s":
        with hypatia_cities.open(encoding="utf-8") as f:
            source = json.load(f)
        keep = {c["name"] for c in source["cities"][:10]}
        data = {
            "cities": [c for c in source["cities"] if c["name"] in keep],
            "pairs": [p for p in source["pairs"] if p["from"] in keep and p["to"] in keep],
        }
    elif case.name == "bariloche-orbit":
        data = {
            "cities": [city_record(*BARILOCHE), city_record(*BEIJING)],
            "pairs": [{"from": BARILOCHE[0], "to": BEIJING[0], "role": "study"}],
        }
    else:
        with cities_file.open(encoding="utf-8") as f:
            data = json.load(f)
        if case.setup is ideal.BARILOCHE:
            data["cities"] += [city_record(*BARILOCHE), city_record(*BEIJING)]
            data["pairs"].append({"from": BARILOCHE[0], "to": BEIJING[0], "role": "study"})
    path = work / f"cities-{case.name}.json"
    with path.open("w", encoding="utf-8") as f:
        json.dump(data, f)
    cities = {c["name"]: City(c["name"], c["lat"], c["lon"]) for c in data["cities"]}
    return path, cities, directions_of(data, case.forward_only)


def run_case(case: Case, exe: Path, args, work: Path) -> list[str]:
    snapshot = work / case.setup.key
    if not snapshot.exists():
        ideal.write_ideal_shell(snapshot, st.DEFAULT_SNAPSHOT / st.EOP_FILE, case.setup)
    cities_path, cities, directions = prepare_cities(case, args.cities, args.hypatia_cities, work)
    print(
        f"\n{case.name}: {case.setup.shell_name}, {len(directions)} directions, "
        f"{st.seconds_text(case.duration_ns)} s every {st.seconds_text(case.step_ns)} s, "
        f"re-plan every {st.seconds_text(case.replan_ns)} s, {', '.join(case.strategies)}"
    )
    summary, program_rows = run_routing(
        exe, snapshot, args.ground, cities_path, case, work / f"{case.name}.csv"
    )
    if "checks: PASSED" not in summary:
        return [f"{case.name}: the program's own checks did not pass"]

    steps = case.duration_ns // case.step_ns + 1
    lookahead = VIEW_HORIZON_NS // case.step_ns if "longest-lived" in case.strategies else 1
    start = st.parse_utc(case.start)
    times = [start.plus_ns(k * case.step_ns) for k in range(steps + lookahead)]
    sats = satrecs(case.setup)
    eop = load_earth_orientation(snapshot / st.EOP_FILE)
    positions = positions_itrf_km(sats, eop, times)
    sample = [times[0], times[steps - 1], times[-1]]
    sample_index = [0, steps - 1, len(times) - 1]
    difference = skyfield_difference_m(sats, snapshot, sample, positions[sample_index])
    print(f"  positions against Skyfield: largest difference {difference * 1e3:.3f} mm")
    problems = []
    if difference > POSITION_CHECK_M:
        problems.append(f"{case.name}: positions {difference:.4f} m from Skyfield's")

    sat_ids = [x.norad_cat_id for x in ideal.all_slots(case.setup)]
    index = {x: i for i, x in enumerate(sat_ids)}
    with (snapshot / ideal.LINKS_FILE).open(encoding="utf-8") as f:
        lines = [line.strip() for line in f if not line.startswith("#")]
    links = np.array([[index[int(x)] for x in line.split(",")] for line in lines[1:]])

    model = Model(case, cities, directions, positions, links)
    model_rows = model.run(steps)
    problems += compare(case, program_rows, model_rows, times[:steps], summary, sat_ids)
    describe(case, model_rows, times[:steps], program_rows)
    return problems


def main(argv: list[str]) -> int:
    st.forbid_network()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--routing", type=Path, required=True)
    parser.add_argument("--cities", type=Path, default=st.REPO_ROOT / "data/ground/cities.json")
    parser.add_argument(
        "--hypatia-cities",
        type=Path,
        default=st.REPO_ROOT / "data/ground/hypatia/cities.json",
    )
    parser.add_argument("--ground", type=Path, default=st.REPO_ROOT / "data/ground/2026-11-19")
    parser.add_argument(
        "--cases",
        default=",".join(c.name for c in CASES),
        help=f"comma-separated, of {', '.join(c.name for c in CASES + BY_HAND)} (default: "
        "the CI cases)",
    )
    args = parser.parse_args(argv)
    if not args.routing.is_file():
        print(f"error: {args.routing} not found", file=sys.stderr)
        return 2
    chosen = args.cases.split(",")
    unknown = set(chosen) - {c.name for c in CASES + BY_HAND}
    if unknown:
        print(f"error: unknown cases {sorted(unknown)}", file=sys.stderr)
        return 2

    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="satsim-validate-routing-") as work_dir:
        for case in CASES + BY_HAND:
            if case.name in chosen:
                problems += run_case(case, args.routing, args, Path(work_dir))

    if problems:
        print(f"\nFAIL: {len(problems)} problems", file=sys.stderr)
        for line in problems[:40]:
            print(f"  {line}", file=sys.stderr)
        return 1
    print(
        "\nPASS: every row (status, cause, route, plan, delays within 1e-9 ms) and every "
        "change count agree with the independent model"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

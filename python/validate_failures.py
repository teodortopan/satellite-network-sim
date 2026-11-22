#!/usr/bin/env python3
"""Cross-check the failures program against an independent model (docs/phase-4-failures.md 6.3).

Written from the Phase 4 spec alone, sharing no code with the C++ program (and written without
reading its Phase 4 sources). Best case only, as the spec says: the realistic model and gateway
loss rest on the program's own checks. Every place where the spec leaves a choice open is marked
in the code with a comment starting `SPEC-GAP:`, with the reading taken here.

What is modelled:

  world       Hypatia's ideal shell (python/make_ideal_shell.py) with one of two four-column
              link files: its +Grid (links-plus-grid-sides.csv) or this project's `three` plan
              (links-three.csv, made by make_ideal_shell.three_plan). Positions by construction
              with validate_routing's own SGP4 and TEME-to-ITRF code; laser line of sight 80 km
              above 6378.137 km; ground links by Hypatia's range rule (--max-ground-range-km),
              for the cities of the run and for every operational gateway (gateways.json, then
              assumed_gateways.json), all at height 0 on the WGS-84 ellipsoid. A tiny hand-made
              world (ToyWorld) runs the same model in the self-tests.
  schedule    the failure file of 4.2, parsed here; what is down at an instant; old news
              (4.2), transient returns.
  timeline    one pass in time order over failure starts and ends, acquisition ends and, with
              --repoint on, the matching steps (4.8): every link's status over time (in force,
              acquiring, failed, off), every repair link (which lasers pair, the step it is
              matched, the instant it carries traffic, drops on a return), and every fact (4.4:
              its subject, down or up, its instant, who detects it and when, deferred detection
              for a link whose geometry was down).
  knowledge   per fact: each satellite detector's report to the nearest working gateway over
              the true network at its detection instant (lasers, then one radio hop down, every
              hop delay + --hop-delay-ms + --hop-processing-s); the planner's knowing instant
              (4.5 planner); with flooding, every node's learning instant as one shortest path
              from all detectors at once on the messages graph of the step the flood starts in,
              and the flood's message count.
  views       which fact about an element wins (the latest), when a view takes a satellite as
              down (4.4).
  lanes       per direction and strategy: `baseline` (no failures), `reference` (a planner that
              knows the true network at every plan), `planner`, `local-repair`, and `flooding`
              (lowest-delay only); the `start` and `whole-interval` planners of Phase 3;
              lowest-delay and keep-until-broken; --replan-on-report. Each lane is evaluated at
              every instant at which anything can change (steps, plans, detections, reports,
              learning plus compute time, returns, acquisition ends, repair instants) and written
              as intervals in continuous time (4.3, 4.6).
  measures    4.7 per lane; after recovery: reachability and the largest piece by union-find,
              and the extra delay of the reachable pairs.

What is compared (exit 1 otherwise; spec 6.3, times to 1e-9 s, delays to 1e-9 ms): every
interval of the events CSV (state, route, cause, element, start, end, delay), every fact of the
facts CSV (subject, state, instant, old, detectors and their instants, planner_known_s, reports,
report_hops, flood_nodes, flood_last_s, messages), every row of the floods CSV, every routes CSV
row's state, cause, element, route, delay, ideal delay (the lane's own true network) and
plan, every row of the measures CSV (to 1e-9) and every row of the network CSV.

Cases (CASES below; Hypatia's shell from its epoch, 1 s steps, re-plan every 15 s, Hypatia's
ground range, New York -> London and Frankfurt -> Singapore one way, lowest-delay and
keep-until-broken, all three reaction modes; the failures are picked from the baseline route
at 7 s and written as failure files):
  satellite-three  `three` plan, the route's middle satellite, 40 s, whole-interval
  laser-grid       +Grid, the route's middle laser link, 40 s, whole-interval
  transient-three  `three`, the middle laser link for 10 s, acquisition 5 s
  repoint-three    `three`, two satellites whose cross-plane partners face each other,
                   --repoint on, acquisition 2 s (in-plane and cross-plane repair links)
  report-three     `three`, the middle satellite, --replan-on-report on, `start`, 25 s
  after-recovery   one step, 3% of the satellites failed an hour before, --repoint on

Not modelled: the realistic endpoint model, gateway failures, the `longest-lived` and
`threshold` strategies, links between shells.

Usage (inside the CI image):

    python3 python/validate_failures.py --self-test
    python3 python/validate_failures.py --failures-program build/validate/bin/failures

Reads only local files; any network access raises.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import heapq
import itertools
import json
import math
import random
import statistics
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

import make_ideal_shell as ideal
import snapshot_tools as st
import validate_routing as vr

C_KM_S = vr.C_KM_S
INF = math.inf
TIME_TOLERANCE_S = 1e-9  # Spec 6.3.
DELAY_TOLERANCE_MS = 1e-9
REACTIONS = ("planner", "local-repair", "flooding")
LANES = ("baseline", "reference", *REACTIONS)
STRATEGIES = ("lowest-delay", "keep-until-broken")


# ---------------------------------------------------------------------------------------
# Settings (spec 4.9 and 4.10).
# ---------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Settings:
    steps: int  # Instants 0 .. steps - 1.
    step_s: float = 1.0
    replan_steps: int = 15
    planner: str = "whole-interval"  # or "start"
    strategies: tuple[str, ...] = STRATEGIES
    reactions: tuple[str, ...] = REACTIONS
    replan_on_report: bool = False
    repoint: bool = False
    detection_s: float = 0.1
    hop_processing_s: float = 0.001
    compute_s: float = 0.1
    planner_processing_s: float = 0.0
    acquisition_s: float = 30.0
    hop_delay_s: float = 0.0

    @property
    def end_s(self) -> float:
        # 4.4 Details: the run ends at its last step's instant.
        return (self.steps - 1) * self.step_s

    def step_of(self, t: float) -> int:
        """The step of an instant: the last step at or before it (4.4 Details)."""
        return max(0, math.floor(t / self.step_s))

    def instant(self, k: int) -> float:
        return k * self.step_s

    def first_step_at_or_after(self, t: float) -> int:
        if t <= 0.0:
            return 0
        k = math.ceil(t / self.step_s)
        if (k - 1) * self.step_s >= t:
            k -= 1
        return k

    def plan_steps(self, k: int) -> tuple[int, int]:
        """The steps a plan made in step k looks at: [k, k_end)."""
        if self.planner == "start":
            return k, k + 1
        # One re-plan interval ahead (Phase 3 4.3; 4.5 Details for a plan on a report).
        return k, k + self.replan_steps


# ---------------------------------------------------------------------------------------
# Elements and their text (4.10 Details).
# ---------------------------------------------------------------------------------------
Pair = tuple[int, int]  # Satellite indices, lower first.
Subject = tuple  # ("satellite", index) | ("link", Pair) | ("gateway", name)
NodeKey = tuple  # ("satellite", index) | ("city", name) | ("gateway", name)


def pair_of(a: int, b: int) -> Pair:
    return (a, b) if a < b else (b, a)


def element_text(subject: Subject, ids: list[int]) -> str:
    kind, value = subject
    if kind == "satellite":
        return f"satellite:{ids[value]}"
    if kind == "link":
        lo, hi = sorted((ids[value[0]], ids[value[1]]))
        return f"link:{lo}-{hi}"
    return f"gateway:{value}"


def node_text(node: NodeKey, ids: list[int]) -> str:
    kind, value = node
    return f"satellite:{ids[value]}" if kind == "satellite" else f"{kind}:{value}"


def instant_text(t: float) -> str:
    return "never" if math.isinf(t) else repr(t)


def parse_instant(text: str) -> float:
    return INF if text == "never" else float(text)


# ---------------------------------------------------------------------------------------
# The failure schedule and its file (4.2).
# ---------------------------------------------------------------------------------------
FAILURE_HEADER = "kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s"


@dataclass(frozen=True)
class Failure:
    kind: str  # "satellite" | "laser_link" | "gateway"
    sats: tuple[int, ...]  # Satellite indices: (x,) or the link's pair.
    gateway: str
    start_s: float
    length_s: float | None  # None: permanent.
    line: int = 0

    @property
    def end_s(self) -> float:
        return INF if self.length_s is None else self.start_s + self.length_s

    @property
    def subject(self) -> Subject:
        if self.kind == "satellite":
            return ("satellite", self.sats[0])
        if self.kind == "laser_link":
            return ("link", pair_of(*self.sats))
        return ("gateway", self.gateway)


class FailureFileError(ValueError):
    pass


def parse_failure_file(
    text: str,
    ids: list[int],
    planned: set[Pair],
    gateways: list[str],
    start: st.UtcInstant,
    step_ns: int,
) -> list[Failure]:
    """The failure file of 4.2, refused whole with the line named on any error."""
    index = {x: i for i, x in enumerate(ids)}
    gateway_count: dict[str, int] = {}
    for g in gateways:
        gateway_count[g] = gateway_count.get(g, 0) + 1
    failures: list[Failure] = []
    header_seen = False
    for number, raw in enumerate(text.splitlines(), start=1):
        if raw.startswith("#"):
            continue
        where = f"line {number}"
        if not header_seen:
            if raw != FAILURE_HEADER:
                raise FailureFileError(f"{where}: header is not {FAILURE_HEADER!r}")
            header_seen = True
            continue
        fields = next(csv.reader([raw], strict=True))
        if len(fields) != 6:
            raise FailureFileError(f"{where}: {len(fields)} fields, not 6")
        kind, a, b, gateway, start_utc, length = fields
        try:
            when = st.parse_utc(start_utc)
        except ValueError as e:
            raise FailureFileError(f"{where}: {e}") from None
        offset_ns = when.unix_ns - start.unix_ns
        if offset_ns >= 0 and offset_ns % step_ns:
            raise FailureFileError(f"{where}: {start_utc} is neither a step nor before the start")
        length_s = None
        if length:
            try:
                length_s = float(length)
            except ValueError:
                raise FailureFileError(f"{where}: length {length!r}") from None
            if not math.isfinite(length_s) or length_s < 0:
                raise FailureFileError(f"{where}: negative or bad length {length!r}")

        def sat(text: str, where: str = where) -> int:
            if not text.isdigit() or int(text) not in index:
                raise FailureFileError(f"{where}: {text!r} is not a satellite of the run")
            return index[int(text)]

        if kind == "satellite":
            if b or gateway:
                raise FailureFileError(f"{where}: a satellite takes only norad_cat_id_a")
            sats: tuple[int, ...] = (sat(a),)
        elif kind == "laser_link":
            if gateway:
                raise FailureFileError(f"{where}: a laser link takes no gateway")
            sats = (sat(a), sat(b))
            if pair_of(*sats) not in planned:
                raise FailureFileError(f"{where}: {a}-{b} is not in the laser plan")
        elif kind == "gateway":
            if a or b:
                raise FailureFileError(f"{where}: a gateway takes no catalogue number")
            if gateway_count.get(gateway, 0) != 1:
                raise FailureFileError(f"{where}: gateway {gateway!r} unknown or not unique")
            sats = ()
        else:
            raise FailureFileError(f"{where}: unknown kind {kind!r}")
        failures.append(
            Failure(kind, sats, gateway, offset_ns / st.NS_PER_S, length_s, line=number)
        )
    if not header_seen:
        raise FailureFileError("no header")
    by_subject: dict[Subject, list[Failure]] = {}
    for f in failures:
        by_subject.setdefault(f.subject, []).append(f)
    for group in by_subject.values():
        group.sort(key=lambda f: f.start_s)
        for x, y in itertools.pairwise(group):
            if y.start_s < x.end_s:
                raise FailureFileError(f"line {y.line}: overlaps the failure on line {x.line}")
    return failures


def write_failure_file(path: Path, failures: list[Failure], ids: list[int], start) -> None:
    lines = ["# written by python/validate_failures.py", FAILURE_HEADER]
    for f in failures:
        utc = start.plus_ns(round(f.start_s * st.NS_PER_S)).iso()
        a = str(ids[f.sats[0]]) if f.sats else ""
        b = str(ids[f.sats[1]]) if len(f.sats) > 1 else ""
        gateway = '"' + f.gateway.replace('"', '""') + '"' if f.gateway else ""
        length = "" if f.length_s is None else repr(f.length_s)
        lines.append(f"{f.kind},{a},{b},{gateway},{utc},{length}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class Schedule:
    """What is down at an instant, from the failures alone."""

    def __init__(self, failures: list[Failure]):
        self.failures = failures
        self.by_subject: dict[Subject, list[Failure]] = {}
        for f in failures:
            self.by_subject.setdefault(f.subject, []).append(f)

    def active(self, subject: Subject, t: float) -> Failure | None:
        for f in self.by_subject.get(subject, ()):
            if f.start_s <= t < f.end_s:
                return f
        return None

    def sat_down(self, x: int, t: float) -> bool:
        return self.active(("satellite", x), t) is not None

    def link_failed(self, pair: Pair, t: float) -> bool:
        return self.active(("link", pair), t) is not None


# ---------------------------------------------------------------------------------------
# Worlds: the geometry the model runs on.
# ---------------------------------------------------------------------------------------
@dataclass(frozen=True)
class PlannedLink:
    a: int
    b: int
    side_a: str
    side_b: str


class World:
    """What the model needs to know of the geometry. Delays are length / c, in seconds."""

    ids: list[int]
    planes: list[list[int]]  # Each plane's satellites in order along the orbit.
    shell_of_plane: list[int]
    shell_names: list[str]
    lasers: list[tuple[str, ...]]  # Per satellite: the sides of its lasers.
    planned: list[PlannedLink]
    cities: list[str]
    gateways: list[str]

    @property
    def n(self) -> int:
        return len(self.ids)

    def plane_of(self, x: int) -> int:
        if not hasattr(self, "_plane_of"):
            self._plane_of = {y: p for p, plane in enumerate(self.planes) for y in plane}
        return self._plane_of[x]

    def plane_east(self, plane: int, offset: int) -> int | None:
        """The plane `offset` planes to the east in the same shell (Phase 2 spec 4.3)."""
        shell = self.shell_of_plane[plane]
        same = [p for p in range(len(self.planes)) if self.shell_of_plane[p] == shell]
        if len(same) < 2 or (offset == 2 and len(same) < 3):
            return None
        return same[(same.index(plane) + offset) % len(same)]

    def laser_delay_s(self, pair: Pair, k: int) -> float | None:
        raise NotImplementedError

    def ground_s(self, site: str, k: int) -> dict[int, float]:
        raise NotImplementedError

    def distance_km(self, pair: Pair, k: int) -> float:
        raise NotImplementedError

    def repair_ok(self, pair: Pair, k: int) -> bool:
        raise NotImplementedError


PATTERN_LASERS = {
    "three": None,  # ahead, behind and one cross-plane laser by parity (ideal.cross_side).
    "plus-grid": ("ahead", "behind", "east", "west"),
}


def read_sided_links(path: Path) -> list[ideal.SidedLink]:
    lines = [x for x in path.read_text(encoding="utf-8").splitlines() if not x.startswith("#")]
    if lines[0] != "norad_cat_id_a,norad_cat_id_b,side_a,side_b":
        raise SystemExit(f"error: {path}: not a four-column link file")
    out = []
    for x in lines[1:]:
        a, b, sa, sb = x.split(",")
        out.append(ideal.SidedLink(int(a), int(b), sa, sb))
    return out


@dataclass(frozen=True)
class Site:
    name: str
    city: vr.City  # Its ground position (any site, not only cities).


def load_gateways(ground: Path) -> list[Site]:
    """Operational gateways: gateways.json, then assumed_gateways.json (Phase 2 order)."""
    out = []
    for name in ("gateways.json", "assumed_gateways.json"):
        path = ground / name
        if not path.is_file():
            continue
        for g in json.loads(path.read_text(encoding="utf-8")):
            if g["status"] == "operational":
                out.append(Site(g["name"], vr.City(g["name"], g["lat"], g["lon"])))
    return out


class IdealWorld(World):
    """Hypatia's ideal shell, best case, Hypatia's ground-range rule."""

    def __init__(
        self,
        setup: ideal.Setup,
        sided: list[ideal.SidedLink],
        pattern: str,
        cities: list[Site],
        gateways: list[Site],
        times: list[st.UtcInstant],
        max_range_km: float,
    ):
        slots = ideal.all_slots(setup)
        self.setup = setup
        self.ids = [x.norad_cat_id for x in slots]
        index = {x: i for i, x in enumerate(self.ids)}
        n = ideal.NUM_SATS_PER_ORB
        self.planes = [[o * n + s for s in range(n)] for o in range(ideal.NUM_ORBS)]
        self.shell_of_plane = [0] * ideal.NUM_ORBS
        self.shell_names = [setup.shell_name]
        if pattern == "three":
            self.lasers = [("ahead", "behind", ideal.cross_side(x)) for x in slots]
        else:
            self.lasers = [PATTERN_LASERS[pattern]] * len(slots)  # type: ignore[list-item]
        self.planned = [PlannedLink(index[x.a], index[x.b], x.side_a, x.side_b) for x in sided]
        self.cities = [x.name for x in cities]
        self.gateways = [x.name for x in gateways]
        self.sites = {x.name: x.city for x in cities + gateways}
        self.max_range_km = max_range_km
        self.times = times
        sats = vr.satrecs(setup)
        eop = vr.load_earth_orientation(st.DEFAULT_SNAPSHOT / st.EOP_FILE)
        self.positions = vr.positions_itrf_km(sats, eop, times)
        self.records = [ideal.satrec_of(x) for x in slots]
        reference = times[0]
        self.circles = [ideal.orbit_circle(r, setup, reference) for r in self.records]
        self._laser: dict[tuple[Pair, int], float | None] = {}
        self._ground: dict[tuple[str, int], dict[int, float]] = {}
        self._u: dict[int, list[float]] = {}

    def laser_delay_s(self, pair: Pair, k: int) -> float | None:
        key = (pair, k)
        if key not in self._laser:
            a, b = self.positions[k][pair[0]], self.positions[k][pair[1]]
            ok = ideal.segment_clears(tuple(a), tuple(b))
            self._laser[key] = float(np.linalg.norm(b - a)) / C_KM_S if ok else None
        return self._laser[key]

    def ground_s(self, site: str, k: int) -> dict[int, float]:
        key = (site, k)
        if key not in self._ground:
            d = self.positions[k] - self.sites[site].position_km
            length = np.sqrt(d[:, 0] * d[:, 0] + d[:, 1] * d[:, 1] + d[:, 2] * d[:, 2])
            idx = np.flatnonzero(length <= self.max_range_km)
            self._ground[key] = {int(s): float(length[s]) / C_KM_S for s in idx}
        return self._ground[key]

    def distance_km(self, pair: Pair, k: int) -> float:
        return float(np.linalg.norm(self.positions[k][pair[1]] - self.positions[k][pair[0]]))

    def repair_ok(self, pair: Pair, k: int) -> bool:
        # "Permanent" at a later step (4.8): each satellite's mean circle from the plan time
        # (Phase 2 4.3) and its argument of latitude at that step.
        if self.laser_delay_s(pair, k) is None:
            return False
        if k not in self._u:
            self._u[k] = [
                c.argument_of_latitude_rad(ideal.teme_state(r, self.times[k])[0])
                for c, r in zip(self.circles, self.records, strict=True)
            ]
        u = self._u[k]
        a, b = pair
        return ideal.permanent(self.circles[a], u[a], self.circles[b], u[b])


class ToyWorld(World):
    """A hand-made world for the self-tests: fixed delays, links down at given steps."""

    def __init__(
        self,
        planes: list[list[int]],
        lasers: list[tuple[str, ...]],
        planned: list[PlannedLink],
        laser_ms: dict[Pair, float],
        ground_ms: dict[str, dict[int, float]],
        cities: list[str],
        gateways: list[str],
        down_steps: dict[Pair, set[int]] | None = None,
        repairable: set[Pair] | None = None,
        distance: dict[Pair, float] | None = None,
    ):
        count = sum(len(p) for p in planes)
        self.ids = [100 + i for i in range(count)]
        self.planes = planes
        self.shell_of_plane = [0] * len(planes)
        self.shell_names = ["toy-s1"]
        self.lasers = lasers
        self.planned = planned
        self.laser_ms = laser_ms
        self.ground_ms = ground_ms
        self.cities = cities
        self.gateways = gateways
        self.down_steps = down_steps or {}
        self.repairable = repairable or set()
        self.distance = distance or {}

    def laser_delay_s(self, pair: Pair, k: int) -> float | None:
        if pair not in self.laser_ms or k in self.down_steps.get(pair, ()):
            return None
        return self.laser_ms[pair] / 1000.0

    def ground_s(self, site: str, k: int) -> dict[int, float]:
        return {s: ms / 1000.0 for s, ms in self.ground_ms.get(site, {}).items()}

    def distance_km(self, pair: Pair, k: int) -> float:
        return self.distance.get(pair, self.laser_ms.get(pair, 1e9))

    def repair_ok(self, pair: Pair, k: int) -> bool:
        return pair in self.repairable and self.laser_delay_s(pair, k) is not None


# ---------------------------------------------------------------------------------------
# Graphs: Dijkstra with the tie rule, the flood, union-find.
# ---------------------------------------------------------------------------------------
Adjacency = dict[int, list[tuple[int, float]]]


@dataclass
class Tree:
    delay: dict[int, float]
    hops: dict[int, int]
    pred: dict[int, int]  # -1 for the source.


def dijkstra(adj: Adjacency, source: int, banned: Iterable[int] = ()) -> Tree:
    """Shortest paths from `source` with Phase 3's tie rule (4.5 there): lowest delay (formed
    as predecessor's delay + edge delay, compared exactly), then fewest hops, then the
    lowest-numbered predecessor."""
    blocked = set(banned)
    delay, hops, pred = {source: 0.0}, {source: 0}, {source: -1}
    heap = [(0.0, 0, source)]
    done: set[int] = set()
    while heap:
        d, h, u = heapq.heappop(heap)
        if u in done or d != delay[u] or h != hops[u]:
            continue
        done.add(u)
        for v, w in adj.get(u, ()):
            if v in done or v in blocked:
                continue
            nd = d + w
            if (nd, h + 1, u) < (delay.get(v, INF), hops.get(v, 0), pred.get(v, -1)):
                delay[v], hops[v], pred[v] = nd, h + 1, u
                heapq.heappush(heap, (nd, h + 1, v))
    return Tree(delay, hops, pred)


def path_to(tree: Tree, target: int) -> list[int] | None:
    if target not in tree.pred:
        return None
    path = [target]
    while tree.pred[path[-1]] != -1:
        path.append(tree.pred[path[-1]])
    return path[::-1]


def flood_times(
    adj: dict[int, list[tuple[int, float]]], sources: dict[int, float]
) -> tuple[dict[int, float], dict[int, int | None]]:
    """Learning instants from several sources at once: each source starts at its own instant,
    each link costs its weight (4.5 flooding). Written directly, not from dijkstra()."""
    learn = dict(sources)
    first: dict[int, int | None] = dict.fromkeys(sources)
    heap = [(t, node) for node, t in sources.items()]
    heapq.heapify(heap)
    settled: set[int] = set()
    while heap:
        t, u = heapq.heappop(heap)
        if u in settled or t != learn[u]:
            continue
        settled.add(u)
        for v, w in adj.get(u, ()):
            arrival = t + w
            if v not in settled and arrival < learn.get(v, INF):
                learn[v] = arrival
                first[v] = u
                heapq.heappush(heap, (arrival, v))
    return learn, first


class UnionFind:
    def __init__(self, items: Iterable[int]):
        self.parent = {x: x for x in items}
        self.size = dict.fromkeys(self.parent, 1)

    def find(self, x: int) -> int:
        root = x
        while self.parent[root] != root:
            root = self.parent[root]
        while self.parent[x] != root:
            self.parent[x], x = root, self.parent[x]
        return root

    def union(self, a: int, b: int) -> None:
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return
        if self.size[ra] < self.size[rb]:
            ra, rb = rb, ra
        self.parent[rb] = ra
        self.size[ra] += self.size[rb]

    def pieces(self) -> list[int]:
        """Sizes of the pieces, largest first."""
        return sorted((self.size[x] for x in self.parent if self.find(x) == x), reverse=True)


def bfs_pieces(nodes: Iterable[int], edges: Iterable[tuple[int, int]]) -> list[int]:
    """The same as UnionFind.pieces by breadth-first search, for the self-test."""
    adj: dict[int, list[int]] = {x: [] for x in nodes}
    for a, b in edges:
        adj[a].append(b)
        adj[b].append(a)
    seen: set[int] = set()
    sizes = []
    for x in adj:
        if x in seen:
            continue
        seen.add(x)
        queue, size = [x], 0
        while queue:
            u = queue.pop()
            size += 1
            for v in adj[u]:
                if v not in seen:
                    seen.add(v)
                    queue.append(v)
        sizes.append(size)
    return sorted(sizes, reverse=True)


# ---------------------------------------------------------------------------------------
# The timeline: link statuses, repair links and facts (4.2, 4.4, 4.8).
# ---------------------------------------------------------------------------------------
@dataclass
class RunLink:
    a: int
    b: int
    side_a: str
    side_b: str
    repair: bool
    history: list[tuple[float, str]] = field(default_factory=list)
    generation: int = 0
    matched_step: int | None = None
    carry_s: float | None = None

    @property
    def pair(self) -> Pair:
        return pair_of(self.a, self.b)

    def status(self, t: float) -> str:
        i = bisect.bisect_right([x for x, _ in self.history], t) - 1
        return self.history[i][1] if i >= 0 else ("off" if self.repair else "in_force")

    def set(self, t: float, status: str) -> None:
        if self.history and self.history[-1][0] == t:
            self.history[-1] = (t, status)
        else:
            self.history.append((t, status))

    def other(self, x: int) -> tuple[int, str]:
        return (self.b, self.side_b) if x == self.a else (self.a, self.side_a)

    def side_of(self, x: int) -> str:
        return self.side_a if x == self.a else self.side_b


@dataclass
class Fact:
    # 4.4 Details: t_s is when the thing happened, not its first detection.
    subject: Subject
    state: str  # "down" | "up"
    t_s: float
    old: bool
    detectors: list[tuple[NodeKey, float]] = field(default_factory=list)
    failure: Failure | None = None  # The failure that made a down fact.
    number: int = -1
    planner_known_s: float = INF
    reports: int = 0
    report_hops: int | None = None
    learn: dict[NodeKey, float] = field(default_factory=dict)
    flood_nodes: int | None = None
    flood_last_s: float | None = None
    messages: int | None = None
    report_rows: list[tuple[NodeKey, float, int]] = field(default_factory=list)


@dataclass(frozen=True)
class RepairRecord:
    a: int
    b: int
    side_a: str
    side_b: str
    matched_step: int
    carry_s: float


class Timeline:
    """One pass in time order (spec 4.2, 4.4 Details, 4.8 Details)."""

    # 4.4 Details, order at one instant: failures that begin (file order), failures that end,
    # acquisitions that end (link order), then one matching.
    START, END, ACQUIRED, MATCH = range(4)

    def __init__(self, world: World, settings: Settings, schedule: Schedule):
        self.world, self.settings, self.schedule = world, settings, schedule
        self.links: list[RunLink] = [
            RunLink(p.a, p.b, p.side_a, p.side_b, repair=False) for p in world.planned
        ]
        self.planned_pairs = {x.pair: i for i, x in enumerate(self.links)}
        self.sat_links: dict[int, list[int]] = {}
        for i, x in enumerate(self.links):
            self.sat_links.setdefault(x.a, []).append(i)
            self.sat_links.setdefault(x.b, []).append(i)
        self.laser_use: dict[tuple[int, str], int | None] = {}
        for s, sides in enumerate(world.lasers):
            for side in sides:
                self.laser_use[(s, side)] = None
        for i, x in enumerate(self.links):
            self.laser_use[(x.a, x.side_a)] = i
            self.laser_use[(x.b, x.side_b)] = i
        self.never_partnered = {laser for laser, use in self.laser_use.items() if use is None}
        self.eligible: dict[tuple[int, str], float] = {}  # Freed lasers: may be matched from.
        self.freed_by: dict[tuple[int, str], float] = {}  # The instant of the freeing failure.
        self.down: set[int] = set()
        self.failed_links: set[Pair] = set()
        self.facts: list[Fact] = []
        self.repairs: list[RepairRecord] = []
        self.active: dict[Subject, int] = {}  # Failures in force per element.
        self.never_again: set[Pair] = set()  # Ends of repair links lost to a failure.
        self._heap: list = []
        self._seq = itertools.count()
        self._run()

    # -- helpers --
    def push(self, t: float, order: int, payload, rank: int | None = None) -> None:
        seq = next(self._seq)
        heapq.heappush(self._heap, (t, order, seq if rank is None else rank, seq, payload))

    def blocked(self, link: RunLink) -> bool:
        return link.a in self.down or link.b in self.down or link.pair in self.failed_links

    def detection_instant(self, link: RunLink, t_f: float, end_s: float) -> float:
        """4.4 Details: at the failure instant's step if the geometry is up there, else at the
        first later step with it up (never if the failure or the run ends first)."""
        s = self.settings
        k = s.step_of(t_f)
        if self.world.laser_delay_s(link.pair, k) is not None:
            return t_f + s.detection_s
        # 4.4 Details: nothing is worked out after the last step.
        for j in range(k + 1, s.steps):
            if s.instant(j) >= end_s:
                break
            if self.world.laser_delay_s(link.pair, j) is not None:
                return s.instant(j) + s.detection_s
        return INF

    def site_detectors(self, x: int, k: int, t: float) -> list[tuple[NodeKey, float]]:
        out = []
        for city in self.world.cities:
            if x in self.world.ground_s(city, k):
                out.append((("city", city), t))
        for g in self.world.gateways:
            if x in self.world.ground_s(g, k):
                out.append((("gateway", g), t))
        return out

    def free_laser(self, laser: tuple[int, str], eligible: float, freed_by: float) -> None:
        self.laser_use[laser] = None
        self.eligible[laser] = eligible
        self.freed_by[laser] = freed_by

    def take_down(self, i: int, t: float) -> None:
        link = self.links[i]
        link.set(t, "off" if link.repair else "failed")
        link.generation += 1

    # -- the pass --
    def _run(self) -> None:
        s = self.settings
        for f in self.schedule.failures:
            if f.kind == "gateway":
                raise SystemExit("error: gateway failures are not in the independent model")
            # 4.4 Details: a failure that ended at or before the start is ignored.
            if f.end_s <= 0.0:
                continue
            self.push(f.start_s, self.START, f)
            if f.end_s <= s.end_s:
                self.push(f.end_s, self.END, f)
        if s.repoint:
            for k in range(s.steps):
                self.push(s.instant(k), self.MATCH, k)
        while self._heap:
            t, order, _, _, payload = heapq.heappop(self._heap)
            if order == self.START:
                self.start(payload, t)
            elif order == self.END:
                self.end(payload, t)
            elif order == self.ACQUIRED:
                self.acquired(*payload, t)
            else:
                self.match(payload)
        # 4.4 Details: numbered in the order they arise (the element's own fact, then its link
        # facts in link order), which is the order of this pass.
        for number, fact in enumerate(self.facts):
            fact.number = number

    def link_down_fact(self, link: RunLink, t: float, f: Failure, ends: list[int]) -> None:
        """A link in force or acquiring goes silent at t: its working ends detect (4.4)."""
        old = t < 0.0
        fact = Fact(("link", link.pair), "down", t, old, failure=f)
        for y in ends:
            side = link.side_of(y)
            if old:
                self.free_laser((y, side), 0.0, t)
                continue
            d = self.detection_instant(link, t, f.end_s)
            # A detector that is itself down detects nothing (4.4 Details).
            if math.isfinite(d) and self.schedule.sat_down(y, d):
                d = INF
            else:
                fact.detectors.append((("satellite", y), d))
            self.free_laser((y, side), d, t)
        self.facts.append(fact)

    def start(self, f: Failure, t: float) -> None:
        s = self.settings
        old = t < 0.0
        self.active[f.subject] = self.active.get(f.subject, 0) + 1
        if self.active[f.subject] > 1:
            return  # 4.4 Details: already down for another failure, no new fact.
        if f.kind == "satellite":
            x = f.sats[0]
            fact = Fact(("satellite", x), "down", t, old, failure=f)
            if not old:
                fact.detectors = self.site_detectors(x, s.step_of(t), t + s.detection_s)
            self.facts.append(fact)
            for i in self.sat_links.get(x, []):
                link = self.links[i]
                if link.status(t) not in ("in_force", "acquiring"):
                    continue
                y, _ = link.other(x)
                # 4.4 Details: an old failure gives the same facts, all old.
                self.link_down_fact(link, t, f, [y] if y not in self.down else [])
                self.laser_use[(x, link.side_of(x))] = None
                self.eligible[(x, link.side_of(x))] = INF
                if link.repair:
                    # 4.8 Details: it does not come back; its ends are not matched again.
                    self.never_again.add(link.pair)
                self.take_down(i, t)
            self.down.add(x)
        else:
            pair = pair_of(*f.sats)
            i = self.planned_pairs[pair]
            link = self.links[i]
            if link.status(t) in ("in_force", "acquiring"):
                ends = [y for y in (link.a, link.b) if y not in self.down]
                self.link_down_fact(link, t, f, ends)
                self.take_down(i, t)
            # 4.4 Details: a link already down because a satellite of it is down gives no fact.
            self.failed_links.add(pair)

    def end(self, f: Failure, t: float) -> None:
        s = self.settings
        self.active[f.subject] -= 1
        if self.active[f.subject] > 0:
            return  # Another failure of the element is still in force.
        if f.kind == "satellite":
            x = f.sats[0]
            self.down.discard(x)
            fact = Fact(("satellite", x), "up", t, False)
            fact.detectors = self.site_detectors(x, s.step_of(t), t)
            self.facts.append(fact)
            candidates = self.sat_links.get(x, [])
        else:
            self.failed_links.discard(pair_of(*f.sats))
            candidates = [self.planned_pairs[pair_of(*f.sats)]]
        restored: set[tuple[int, str]] = set()
        for i in candidates:
            link = self.links[i]
            if link.repair or link.status(t) != "failed" or self.blocked(link):
                continue
            for laser in ((link.a, link.side_a), (link.b, link.side_b)):
                use = self.laser_use.get(laser)
                if use is not None and self.links[use].repair:
                    self.drop_repair(use, t)
                self.laser_use[laser] = i
                restored.add(laser)
            link.set(t, "acquiring")
            link.generation += 1
            self.push(t + s.acquisition_s, self.ACQUIRED, (i, link.generation), rank=i)
        if f.kind == "satellite":
            x = f.sats[0]
            for side in self.world.lasers[x]:
                laser = (x, side)
                if laser not in restored and self.laser_use.get(laser) is None:
                    # 4.8 Details: a returned satellite's laser still without a link may be
                    # matched from the return.
                    self.eligible[laser] = t
                    self.freed_by[laser] = t

    def drop_repair(self, i: int, t: float) -> None:
        """4.8: a repair link using a laser whose original partner is back is dropped; both
        ends know at once."""
        link = self.links[i]
        self.take_down(i, t)
        fact = Fact(("link", link.pair), "down", t, False)
        for y in (link.a, link.b):
            if y not in self.down:
                fact.detectors.append((("satellite", y), t))
            laser = (y, link.side_of(y))
            if self.laser_use.get(laser) == i:
                self.free_laser(laser, t, t)
        self.facts.append(fact)

    def acquired(self, i: int, generation: int, t: float) -> None:
        link = self.links[i]
        if link.generation != generation or link.status(t) != "acquiring":
            return
        link.set(t, "in_force")
        if link.repair:
            link.carry_s = t
        fact = Fact(("link", link.pair), "up", t, False)
        # Both ends hold "link up" at the end of the acquisition, whatever the geometry.
        fact.detectors = [(("satellite", y), t) for y in (link.a, link.b)]
        self.facts.append(fact)

    def excluded(self, pair: Pair, t: float) -> bool:
        """4.8 Details: not the two ends of a failed link while it is failed, and not a pair
        already joined by a link that carries traffic or is acquiring."""
        i = self.planned_pairs.get(pair)
        if i is not None and self.blocked(self.links[i]):
            return True
        if pair in self.never_again:
            return True
        return any(x.pair == pair and x.status(t) in ("in_force", "acquiring") for x in self.links)

    def match(self, k: int) -> None:
        s = self.settings
        t = s.instant(k)
        pool = {
            laser
            for laser, use in self.laser_use.items()
            if use is None
            and laser[0] not in self.down
            and (laser in self.never_partnered or self.eligible.get(laser, INF) <= t)
        }
        trigger = any(
            laser not in self.never_partnered
            and s.first_step_at_or_after(self.eligible[laser]) == k
            for laser in pool
        )
        if not trigger:
            return
        made: list[tuple[int, str, int, str]] = []
        # In a plane (4.8 Details): round the plane's working satellites in order.
        for plane in self.world.planes:
            working = [x for x in plane if x not in self.down]
            if len(working) < 2:
                continue
            for i, x in enumerate(working):
                y = working[(i + 1) % len(working)]
                a_laser, b_laser = (x, "ahead"), (y, "behind")
                if a_laser not in pool or b_laser not in pool:
                    continue
                pair = pair_of(x, y)
                if self.excluded(pair, t) or not self.world.repair_ok(pair, k):
                    continue
                pool -= {a_laser, b_laser}
                made.append((x, "ahead", y, "behind"))
        # Across planes: Phase 2's matching on the free lasers only.
        ids = self.world.ids
        for east_side, west_side, offset in (
            ("east", "west", 1),
            ("second_east", "second_west", 2),
        ):
            for p, plane in enumerate(self.world.planes):
                q = self.world.plane_east(p, offset)
                if q is None:
                    continue
                easts = [x for x in plane if (x, east_side) in pool]
                wests = [y for y in self.world.planes[q] if (y, west_side) in pool]
                candidates = sorted(
                    (self.world.distance_km(pair_of(x, y), k), ids[x], ids[y], x, y)
                    for x in easts
                    for y in wests
                    if x != y
                )
                for _, _, _, x, y in candidates:
                    if (x, east_side) not in pool or (y, west_side) not in pool:
                        continue
                    pair = pair_of(x, y)
                    if self.excluded(pair, t) or not self.world.repair_ok(pair, k):
                        continue
                    pool -= {(x, east_side), (y, west_side)}
                    made.append((x, east_side, y, west_side))
        for x, side_x, y, side_y in made:
            self.add_repair(x, side_x, y, side_y, k)

    def add_repair(self, x: int, side_x: str, y: int, side_y: str, k: int) -> None:
        s = self.settings
        t = s.instant(k)
        link = RunLink(x, y, side_x, side_y, repair=True, matched_step=k)
        i = len(self.links)
        self.links.append(link)
        self.sat_links.setdefault(x, []).append(i)
        self.sat_links.setdefault(y, []).append(i)
        self.laser_use[(x, side_x)] = i
        self.laser_use[(y, side_y)] = i
        carry = t + s.acquisition_s
        happened = carry
        if k == 0:
            # 4.8 Details, old failures: carries from the failure instant plus the acquisition
            # time, or from the first step if that is earlier.
            # 4.8 Details: the later of the two failure instants counts; a laser that never had
            # a partner does not count, and a link between two such lasers carries from step 0.
            freed = [
                self.freed_by.get(laser, -INF) if laser not in self.never_partnered else -INF
                for laser in ((x, side_x), (y, side_y))
            ]
            if all(f < 0.0 for f in freed):
                happened = max(freed) + s.acquisition_s
                carry = max(happened, 0.0)
        link.carry_s = carry
        self.repairs.append(RepairRecord(x, y, side_x, side_y, k, carry))
        if carry <= t:
            link.set(t, "in_force")
            # SPEC-GAP: the instant of an old repair link's "up" fact is when its acquisition
            # ended (the failure instant plus the acquisition time, before the start), as the
            # program writes it; 0 for a link between two lasers that never had a partner.
            t_s = happened if math.isfinite(happened) else t
            self.facts.append(Fact(("link", link.pair), "up", t_s, True))
            return
        link.set(t, "acquiring")
        link.generation += 1
        self.push(carry, self.ACQUIRED, (i, link.generation), rank=i)


# ---------------------------------------------------------------------------------------
# The model.
# ---------------------------------------------------------------------------------------
@dataclass(frozen=True)
class View:
    down_sats: frozenset[int]
    present: frozenset[Pair]  # Laser pairs the actor believes work (ends not down).


@dataclass(frozen=True)
class Sample:
    """A lane's state at an instant (4.6)."""

    state: str  # delivered | black_hole | loop | no_route
    cause: str = ""
    element: str = ""
    route: tuple[int, ...] = ()  # Satellite indices.
    delay_s: float | None = None

    def key(self):
        return (self.state, self.cause, self.element, self.route)


@dataclass(frozen=True)
class Interval:
    start_s: float
    end_s: float
    sample: Sample


@dataclass
class Lane:
    kind: str  # A LANES entry.
    src: str
    dst: str
    strategy: str
    route: list[int] | None = None  # Nodes, dish to dish.
    plan_cause: str = "no_path"
    plan_s: float = 0.0
    backups: list[list[int] | None] = field(default_factory=list)
    repaired: bool = False
    repair_edge: tuple[int, int] | None = None
    repair_failure: Failure | None = None
    samples: list[tuple[float, Sample]] = field(default_factory=list)
    plan_marks: dict[int, str] = field(default_factory=dict)


@dataclass(frozen=True)
class Direction:
    src: str
    dst: str
    role: str


class Model:
    def __init__(
        self,
        world: World,
        settings: Settings,
        failures: list[Failure],
        directions: list[Direction],
    ):
        self.world, self.s, self.directions = world, settings, directions
        self.schedule = Schedule(failures)
        self.timeline = Timeline(world, settings, self.schedule)
        self.facts = self.timeline.facts
        self.links = self.timeline.links
        self.n = world.n
        self.city_index = {c: i for i, c in enumerate(world.cities)}
        self.planned_pairs = frozenset(x.pair for x in self.links if not x.repair)
        self.sat_planned: dict[int, list[Pair]] = {}
        for p in self.planned_pairs:
            for x in p:
                self.sat_planned.setdefault(x, []).append(p)
        self._graphs: dict = {}
        self._views: dict[frozenset[int], View] = {}
        self._trees: dict = {}
        self._carrying: dict[int, frozenset[Pair]] = {}
        self._changes: list[float] | None = None
        self._msg_graphs: dict = {}
        self.flooding = "flooding" in settings.reactions
        self.know()

    # -- nodes of the routing graph (Phase 3 spec 4.2 numbering) --
    def send(self, city: str) -> int:
        return self.n + 2 * self.city_index[city]

    def recv(self, city: str) -> int:
        return self.n + 2 * self.city_index[city] + 1

    # -- truth --
    def carrying(self, t: float) -> frozenset[Pair]:
        """Pairs with a link in force at t (geometry not yet applied)."""
        if self._changes is None:
            self._changes = sorted({u for x in self.links for u, _ in x.history})
        i = bisect.bisect_right(self._changes, t)
        if i not in self._carrying:
            at = self._changes[i - 1] if i > 0 else -INF
            self._carrying[i] = frozenset(x.pair for x in self.links if x.status(at) == "in_force")
        return self._carrying[i]

    def truth_view(self, t: float) -> View:
        down = frozenset(x for x in range(self.n) if self.schedule.sat_down(x, t))
        present = frozenset(p for p in self.carrying(t) if p[0] not in down and p[1] not in down)
        return View(down, present)

    def baseline_view(self) -> View:
        return View(frozenset(), self.planned_pairs)

    # -- the messages graph (4.4 Details) --
    def message_nodes(self) -> list[NodeKey]:
        return (
            [("satellite", x) for x in range(self.n)]
            + [("city", c) for c in self.world.cities]
            + [("gateway", g) for g in self.world.gateways]
        )

    def message_graph(self, t: float) -> tuple[Adjacency, dict[NodeKey, int], set[int]]:
        """Adjacency on node numbers, the numbering, and the gateway numbers.

        The cities are those of the run's directions; a city's dish passes messages on like
        any node."""
        if t in self._msg_graphs:
            return self._msg_graphs[t]
        s = self.s
        k = s.step_of(t)
        view = self.truth_view(t)
        numbers = {node: i for i, node in enumerate(self.message_nodes())}
        per_hop = s.hop_delay_s + s.hop_processing_s
        adj: Adjacency = {}

        def add(u: int, v: int, w: float) -> None:
            adj.setdefault(u, []).append((v, w))
            adj.setdefault(v, []).append((u, w))

        for p in sorted(view.present):
            d = self.world.laser_delay_s(p, k)
            if d is not None:
                add(p[0], p[1], d + per_hop)
        gateways = set()
        for kind, sites in (("city", self.world.cities), ("gateway", self.world.gateways)):
            for site in sites:
                u = numbers[(kind, site)]
                if kind == "gateway":
                    gateways.add(u)
                for x, d in sorted(self.world.ground_s(site, k).items()):
                    if x not in view.down_sats:
                        add(u, x, d + per_hop)
        self._msg_graphs[t] = (adj, numbers, gateways)
        return self._msg_graphs[t]

    def report(self, node: NodeKey, t: float) -> tuple[float, int] | None:
        """4.4 Details: a report from a detector at t: the lowest total, over paths on the
        messages graph of the true network at t to any working gateway, of every link's delay
        plus the per-hop time; and that path's hops. None when no path exists."""
        adj, numbers, gateways = self.message_graph(t)
        if node not in numbers:
            return None
        tree = dijkstra(adj, numbers[node])
        reached = [(tree.delay[g], tree.hops[g]) for g in gateways if g in tree.delay]
        if not reached:
            return None
        travel, hops = min(reached)
        return t + travel, hops

    def know(self) -> None:
        """Reports, the planner's knowing instant and the floods of every fact."""
        s = self.s
        names = self.message_nodes()
        for fact in self.facts:
            if fact.old:
                fact.planner_known_s = 0.0
                if self.flooding:
                    fact.flood_nodes, fact.flood_last_s, fact.messages = 0, INF, 0
                continue
            for node, d in fact.detectors:
                # 4.4 Details: a detection after the last step sends no report.
                if d > s.end_s:
                    continue
                if node[0] == "gateway":
                    # 4.10 Details: a gateway that detects is one report with no hops.
                    r: tuple[float, int] | None = (d, 0)
                else:
                    r = self.report(node, d)
                if r is not None:
                    fact.reports += 1
                    fact.report_rows.append((node, r[0], r[1]))
            if fact.report_rows:
                # 4.10 Details: of reports arriving together the one with fewer hops is first.
                first, fact.report_hops = min((a, h) for _, a, h in fact.report_rows)
                fact.planner_known_s = first + s.planner_processing_s
            if not self.flooding:
                continue
            # 4.4 Details: a detection after the last step starts no flood.
            starts = [d for _, d in fact.detectors if d <= s.end_s]
            if not starts:
                # 4.10 Details: a fact nobody detects in the run: 0 nodes, never, 0 messages.
                fact.flood_nodes, fact.flood_last_s, fact.messages = 0, INF, 0
                continue
            t0 = min(starts)
            adj, numbers, _ = self.message_graph(t0)
            sources: dict[int, float] = {}
            for node, d in fact.detectors:
                if d <= s.end_s and node in numbers:
                    sources[numbers[node]] = min(d, sources.get(numbers[node], INF))
            learn, first = flood_times(adj, sources)
            fact.learn = {names[u]: t for u, t in learn.items()}
            fact.flood_nodes = len(learn)
            fact.flood_last_s = max(learn.values())
            fact.messages = sum(len(adj.get(u, ())) - (0 if first[u] is None else 1) for u in learn)

    # -- views (4.4) --
    def resolve(self, known: frozenset[int]) -> View:
        if known not in self._views:
            self._views[known] = self._resolve(known)
        return self._views[known]

    def _resolve(self, known: frozenset[int]) -> View:
        latest: dict[Subject, Fact] = {}
        for i in known:
            f = self.facts[i]
            old = latest.get(f.subject)
            if old is None or (f.t_s, f.number) > (old.t_s, old.number):
                latest[f.subject] = f
        down = {
            f.subject[1]
            for f in latest.values()
            if f.subject[0] == "satellite" and f.state == "down"
        }
        down_pairs = {
            f.subject[1] for f in latest.values() if f.subject[0] == "link" and f.state == "down"
        }
        up_pairs = {
            f.subject[1] for f in latest.values() if f.subject[0] == "link" and f.state == "up"
        }
        repair_up = up_pairs - self.planned_pairs
        for x, pairs in self.sat_planned.items():
            # 4.4: every planned laser link of x went silent with only the other end noticing
            # (latest fact down, not old news, x not among its detectors), and no repair link
            # of x is known up.
            # SPEC-GAP: x listed as a detector with the instant `never` still counts as one of
            # the fact's detectors.
            def silent(p: Pair, x: int = x) -> bool:
                f = latest.get(("link", p))
                return (
                    f is not None
                    and f.state == "down"
                    and not f.old
                    and all(node != ("satellite", x) for node, _ in f.detectors)
                )

            if all(silent(p) for p in pairs) and not any(x in p for p in repair_up):
                down.add(x)
        present = {p for p in self.planned_pairs if p not in down_pairs} | repair_up
        present = {p for p in present if p[0] not in down and p[1] not in down}
        return View(frozenset(down), frozenset(present))

    def planner_view(self, t: float) -> View:
        return self.resolve(
            frozenset(i for i, f in enumerate(self.facts) if f.old or f.planner_known_s <= t)
        )

    def node_view(self, node: NodeKey, t: float) -> View:
        c = self.s.compute_s
        return self.resolve(
            frozenset(
                i for i, f in enumerate(self.facts) if f.old or f.learn.get(node, INF) + c <= t
            )
        )

    # -- planning graphs --
    def graph(self, k0: int, k1: int, view: View) -> tuple[Adjacency, Adjacency]:
        """The planning graph (Phase 3 4.3) of steps [k0, k1) from a view: edges that exist at
        every one of those steps, delays of step k0; forward and reversed."""
        key = (k0, k1, view)
        if key in self._graphs:
            return self._graphs[key]
        hop = self.s.hop_delay_s
        adj: Adjacency = {}
        radj: Adjacency = {}

        def add(u: int, v: int, w: float) -> None:
            adj.setdefault(u, []).append((v, w))
            radj.setdefault(v, []).append((u, w))

        for p in sorted(view.present):
            delays = [self.world.laser_delay_s(p, k) for k in range(k0, k1)]
            if all(d is not None for d in delays):
                add(p[0], p[1], delays[0] + hop)  # type: ignore[operator]
                add(p[1], p[0], delays[0] + hop)  # type: ignore[operator]
        for city in self.world.cities:
            seen = [self.world.ground_s(city, k) for k in range(k0, k1)]
            for x in sorted(seen[0]):
                if x in view.down_sats or not all(x in g for g in seen):
                    continue
                add(self.send(city), x, seen[0][x] + hop)
                add(x, self.recv(city), seen[0][x] + hop)
        self._graphs[key] = (adj, radj)
        return adj, radj

    def tree_towards(self, k0: int, k1: int, view: View, dst: str) -> Tree:
        """4.5 flooding: the shortest-path tree towards the destination, the tie rule applied
        from the destination's side; pred is a node's next hop."""
        key = (k0, k1, view, dst)
        if key not in self._trees:
            _, radj = self.graph(k0, k1, view)
            self._trees[key] = dijkstra(radj, self.recv(dst))
        return self._trees[key]

    def ideal_s(self, lane: Lane, k: int) -> float | None:
        """4.6 Details: the lowest delay in the lane's own true network at step k (the
        baseline's has no failures)."""
        t = self.s.instant(k)
        view = self.baseline_view() if lane.kind == "baseline" else self.truth_view(t)
        adj, _ = self.graph(k, k + 1, view)
        return dijkstra(adj, self.send(lane.src)).delay.get(self.recv(lane.dst))

    def covered(self, d: Direction, k: int) -> bool:
        return bool(self.world.ground_s(d.src, k)) and bool(self.world.ground_s(d.dst, k))

    # -- the truth of one edge --
    def edge(self, d: Direction, u: int, v: int, t: float, baseline: bool):
        """(delay or None, failure subject or None, ground edge?) of edge u -> v at t."""
        k = self.s.step_of(t)
        hop = self.s.hop_delay_s
        sat_down = (lambda x: False) if baseline else (lambda x: self.schedule.sat_down(x, t))
        if u >= self.n:  # Sending dish to a satellite.
            if sat_down(v):
                return None, ("satellite", v), True
            g = self.world.ground_s(d.src, k)
            return (g[v] + hop if v in g else None), None, True
        if v >= self.n:  # A satellite to the receiving dish.
            if sat_down(u):
                return None, ("satellite", u), True
            g = self.world.ground_s(d.dst, k)
            return (g[u] + hop if u in g else None), None, True
        p = pair_of(u, v)
        # 4.6 Details: a satellite that is down is `failed_satellite` even when the link to it
        # has failed too; a failed link with both satellites working is `failed_link`.
        for x in (u, v):
            if sat_down(x):
                return None, ("satellite", x), False
        if not baseline and self.schedule.link_failed(p, t):
            return None, ("link", p), False
        carrying = self.planned_pairs if baseline else self.carrying(t)
        w = self.world.laser_delay_s(p, k) if p in carrying else None
        # 4.6 Details: a link out of sight, acquiring or not in force is not a failure.
        return (None if w is None else w + hop), None, False

    def evaluate(self, d: Direction, route: list[int] | None, t: float, lane: Lane) -> Sample:
        """The state of a held route at t (4.6)."""
        k = self.s.step_of(t)
        baseline = lane.kind == "baseline"
        if route is None:
            cause = lane.plan_cause if self.covered(d, k) else "no_coverage"
            return Sample("no_route", cause)
        sats = tuple(route[1:-1])
        edges = [self.edge(d, u, v, t, baseline) for u, v in itertools.pairwise(route)]
        down = [x for x in edges if x[0] is None]
        if not down:
            total = 0.0
            for w, _, _ in edges:
                total += w  # type: ignore[operator]
            return Sample("delivered", route=sats, delay_s=total)
        # 4.6 Details: a failed element anywhere on the route comes before every other cause
        # (the first one along it); otherwise Phase 3's causes.
        failed = [f for _, f, _ in down if f is not None]
        if failed:
            return failure_sample(failed[0], sats, self.world.ids)
        if not self.covered(d, k):
            return Sample("no_route", "no_coverage", route=sats)
        ground_missing = any(f is None and g for _, f, g in down)
        cause = "ground_link_lost" if ground_missing else "laser_link_down"
        return Sample("no_route", cause, route=sats)

    def walk(self, d: Direction, t: float, plan_k: int) -> Sample:
        """4.5 flooding: follow every node's own table from the sending dish."""
        k = self.s.step_of(t)
        k0, k1 = self.s.plan_steps(plan_k)
        node, end = self.send(d.src), self.recv(d.dst)
        visited = {node}
        sats: list[int] = []
        total = 0.0
        while True:
            key: NodeKey = ("city", d.src) if node >= self.n else ("satellite", node)
            tree = self.tree_towards(k0, k1, self.node_view(key, t), d.dst)
            nxt = tree.pred.get(node)
            if nxt is None or nxt == -1:
                if node == self.send(d.src):
                    # 4.6 Details: a sending dish with no next hop has no route.
                    cause = "no_path" if self.covered(d, k) else "no_coverage"
                    return Sample("no_route", cause)
                return Sample("black_hole", route=tuple(sats))
            w, failure, ground = self.edge(d, node, nxt, t, False)
            if w is None:
                if failure is not None:
                    return failure_sample(failure, tuple(sats), self.world.ids)
                if not self.covered(d, k):
                    return Sample("no_route", "no_coverage", route=tuple(sats))
                cause = "ground_link_lost" if ground else "laser_link_down"
                return Sample("no_route", cause, route=tuple(sats))
            total += w
            if nxt == end:
                return Sample("delivered", route=tuple(sats), delay_s=total)
            if nxt in visited:
                return Sample("loop", route=(*sats, nxt))
            visited.add(nxt)
            sats.append(nxt)
            node = nxt

    # -- plans --
    def plan(self, lane: Lane, d: Direction, t: float, view: View, mark: bool) -> None:
        k = self.s.step_of(t)
        k0, k1 = self.s.plan_steps(k)
        adj, _ = self.graph(k0, k1, view)
        if lane.repaired and lane.repair_edge is not None:
            u, v = lane.repair_edge
            if any(x == v for x, _ in adj.get(u, ())):
                # 4.5 Details: the planning graph still has the dead hop: keep the repaired
                # route, recorded as `kept`; the repair stays used up.
                if mark:
                    lane.plan_marks[k] = "kept"
                return
        old = lane.route
        keep = (
            lane.strategy == "keep-until-broken"
            and old is not None
            and all(any(x == v for x, _ in adj.get(u, ())) for u, v in itertools.pairwise(old))
        )
        if not keep:
            if not self.covered(d, k):
                lane.route, lane.plan_cause = None, "no_coverage"
            else:
                lane.route = path_to(dijkstra(adj, self.send(d.src)), self.recv(d.dst))
                lane.plan_cause = "no_path"
        lane.plan_s = t
        lane.repaired, lane.repair_edge, lane.repair_failure = False, None, None
        if mark:
            if lane.route is None:
                lane.plan_marks[k] = "none"
            else:
                lane.plan_marks[k] = "kept" if lane.route == old else "replaced"
        if lane.kind == "local-repair":
            lane.backups = []
            route = lane.route or []
            for i in range(len(route) - 1):
                banned = [*route[:i], route[i + 1]]
                lane.backups.append(path_to(dijkstra(adj, route[i], banned), self.recv(d.dst)))

    def first_failure(self, d: Direction, route: list[int], t: float):
        """The first failed element along the route at t: (edge index, failure)."""
        for i, (u, v) in enumerate(itertools.pairwise(route)):
            _, failure, _ = self.edge(d, u, v, t, False)
            if failure is not None:
                return i, self.schedule.active(failure, t)
        return None

    def detection_of(self, node: NodeKey, f: Failure, subjects: set) -> float | None:
        for fact in self.facts:
            if fact.failure is f and fact.subject in subjects:
                for x, when in fact.detectors:
                    if x == node:
                        return when
        return None

    def try_repair(self, lane: Lane, d: Direction, t: float) -> None:
        """4.5 local-repair Details."""
        if lane.repaired or lane.route is None:
            return
        found = self.first_failure(d, lane.route, t)
        if found is None:
            return
        i, f = found
        j, nxt = lane.route[i], lane.route[i + 1]
        if j >= self.n:
            node: NodeKey = ("city", d.src)
            subjects = {f.subject}
        else:
            node = ("satellite", j)
            subjects = {f.subject, ("link", pair_of(j, nxt))} if nxt < self.n else {f.subject}
        passes = max(lane.plan_s, f.start_s)
        detected = self.detection_of(node, f, subjects)
        when = passes + self.s.detection_s if detected is None else max(detected, passes)
        if when > t:
            return
        backup = lane.backups[i] if i < len(lane.backups) else None
        lane.repair_edge, lane.repair_failure = (j, nxt), f
        # 4.5 Details: a node with no backup cannot repair; the one repair is used up.
        if backup is not None:
            lane.route = lane.route[:i] + backup
        lane.repaired = True

    # -- the run --
    def instants(self) -> list[float]:
        s = self.s
        out = {s.instant(k) for k in range(s.steps)}
        plans = [s.instant(k) for k in range(0, s.steps, s.replan_steps)]
        out.update(t + s.detection_s for t in plans)
        for f in self.schedule.failures:
            out.update((f.start_s, f.start_s + s.detection_s, f.end_s))
        for fact in self.facts:
            out.add(fact.t_s)
            out.update(t for _, t in fact.detectors)
            out.add(fact.planner_known_s)
            if self.flooding:
                out.update(t + s.compute_s for t in fact.learn.values())
        out.update(t + s.detection_s for t in self.report_plans())
        out.update(self.report_plans())
        for x in self.links:
            out.update(t for t, _ in x.history)
        return sorted(t for t in out if 0.0 <= t <= s.end_s)

    def report_plans(self) -> list[float]:
        """--replan-on-report: every instant the planner learns a fact (4.5 Details: plans for
        `planner` and `local-repair` only)."""
        if not self.s.replan_on_report:
            return []
        return sorted(
            {f.planner_known_s for f in self.facts if not f.old and f.planner_known_s > 0.0}
        )

    def run(self) -> dict[tuple[str, str, str, str], Lane]:
        s = self.s
        lanes: dict[tuple[str, str, str, str], Lane] = {}
        for d in self.directions:
            for strategy in s.strategies:
                for kind in ("baseline", "reference", "planner", "local-repair"):
                    if kind in REACTIONS and kind not in s.reactions:
                        continue
                    lanes[(d.src, d.dst, strategy, kind)] = Lane(kind, d.src, d.dst, strategy)
            if self.flooding:
                lanes[(d.src, d.dst, "lowest-delay", "flooding")] = Lane(
                    "flooding", d.src, d.dst, "lowest-delay"
                )
        by_key = {(d.src, d.dst): d for d in self.directions}
        report_plans = set(self.report_plans())
        planner_views: dict[float, View] = {}
        for t in self.instants():
            k = s.step_of(t)
            regular = t == s.instant(k) and k % s.replan_steps == 0
            for lane in lanes.values():
                d = by_key[(lane.src, lane.dst)]
                if lane.kind == "flooding":
                    lane.samples.append(
                        (t, self.walk(d, t, (k // s.replan_steps) * s.replan_steps))
                    )
                    continue
                on_report = t in report_plans and lane.kind in ("planner", "local-repair")
                if regular or on_report:
                    if lane.kind == "baseline":
                        view = self.baseline_view()
                    elif lane.kind == "reference":
                        view = self.truth_view(t)
                    else:
                        if t not in planner_views:
                            planner_views[t] = self.planner_view(t)
                        view = planner_views[t]
                    self.plan(lane, d, t, view, mark=regular)
                if lane.kind == "local-repair":
                    self.try_repair(lane, d, t)
                lane.samples.append((t, self.evaluate(d, lane.route, t, lane)))
        return lanes


def failure_sample(subject: Subject, sats: tuple[int, ...], ids: list[int]) -> Sample:
    cause = {"satellite": "failed_satellite", "link": "failed_link"}[subject[0]]
    return Sample("black_hole", cause, element_text(subject, ids), sats)


def intervals_of(samples: list[tuple[float, Sample]], end_s: float) -> list[Interval]:
    """Merge consecutive equal states (4.10 Details: an interval ends when the state, the
    route, the cause or the element changes; its delay is the one at its start)."""
    out: list[Interval] = []
    for i, (t, sample) in enumerate(samples):
        if out and out[-1].sample.key() == sample.key():
            continue
        if out:
            out[-1] = Interval(out[-1].start_s, t, out[-1].sample)
        out.append(Interval(t, end_s, sample))
        del i
    return out


def sample_at(intervals: list[Interval], t: float) -> Sample:
    starts = [x.start_s for x in intervals]
    return intervals[max(0, bisect.bisect_right(starts, t) - 1)].sample


# ---------------------------------------------------------------------------------------
# Measures (4.7).
# ---------------------------------------------------------------------------------------
@dataclass
class Measures:
    """One row of the measures CSV (4.10 Details); None is written `none`."""

    failure_s: float | None = None
    black_hole_s: float | None = None
    loop_s: float | None = None
    lost_s: float | None = None
    restore_s: float | None = None
    settle_s: float | None = None
    extra_min_ms: float | None = None
    extra_median_ms: float | None = None
    extra_max_ms: float | None = None
    extra_settled_ms: float | None = None
    reports: int | None = None
    messages: int | None = None


MEASURE_COLUMNS = (
    "failure_s", "black_hole_s", "loop_s", "lost_s", "restore_s", "settle_s", "extra_min_ms",
    "extra_median_ms", "extra_max_ms", "extra_settled_ms", "reports", "messages",
)  # fmt: skip


def failure_instant(failures: list[Failure]) -> float | None:
    """The run's failure instant: the earliest failure that is not old news."""
    fresh = [f.start_s for f in failures if f.start_s >= 0.0]
    return min(fresh) if fresh else None


def measures(
    lane: list[Interval],
    baseline: list[Interval],
    reference: list[Interval],
    t_f: float | None,
    settings: Settings,
    steps: tuple[list[Sample], list[Sample]] | None = None,
) -> Measures:
    """4.7 for one lane; `steps` holds the lane's and the baseline's state at each step (with
    that step's delay, which an interval does not keep). Without a failure that is not old
    news, every measure that counts from t_f is none (4.7)."""
    m = Measures()
    if t_f is None:
        return m
    end_s = settings.end_s
    m.failure_s, m.black_hole_s, m.loop_s, m.lost_s = t_f, 0.0, 0.0, 0.0
    cuts = sorted(
        {t_f, end_s} | {x.start_s for x in lane + baseline + reference if t_f < x.start_s < end_s}
    )
    interrupted = False
    restored_at = None
    last_differ_end = None
    for a, b in itertools.pairwise(cuts):
        mine, base, ref = sample_at(lane, a), sample_at(baseline, a), sample_at(reference, a)
        length = b - a
        if mine.state == "black_hole":
            m.black_hole_s += length
        elif mine.state == "loop":
            m.loop_s += length
        if base.state == "delivered" and mine.state != "delivered":
            m.lost_s += length
        if mine.state != "delivered":
            interrupted = True
        elif interrupted and restored_at is None:
            restored_at = a
        # Settled: the state and the route are the reference lane's.
        if (mine.state, mine.route) != (ref.state, ref.route):
            last_differ_end = b
    if not interrupted:
        m.restore_s = 0.0
    elif restored_at is not None:
        m.restore_s = restored_at - t_f
    if last_differ_end is None:
        m.settle_s = 0.0
    elif last_differ_end < end_s:
        m.settle_s = last_differ_end - t_f

    def extra_at(k: int) -> float | None:
        # 4.10 Details: per step, the lane's delay minus the baseline's at that step.
        if steps is None:
            t = settings.instant(k)
            mine, base = sample_at(lane, t), sample_at(baseline, t)
        else:
            mine, base = steps[0][k], steps[1][k]
        if mine.state == "delivered" and base.state == "delivered":
            return (mine.delay_s - base.delay_s) * 1e3  # type: ignore[operator]
        return None

    # 4.10 Details: at the steps at or after t_f where the lane and the baseline are delivered.
    first = settings.first_step_at_or_after(t_f)
    extras = [x for k in range(first, settings.steps) if (x := extra_at(k)) is not None]
    if extras:
        m.extra_min_ms, m.extra_max_ms = min(extras), max(extras)
        m.extra_median_ms = statistics.median(extras)
    if m.settle_s is not None:
        k = settings.first_step_at_or_after(t_f + m.settle_s)
        if k < settings.steps:
            m.extra_settled_ms = extra_at(k)
    return m


def lane_messages(facts: list[Fact], kind: str) -> tuple[int | None, int]:
    """4.7 messages: in `planner` and `local-repair` the reports sent and their hops; in
    `flooding` every sending of every fact on a link.

    SPEC-GAP: the measures CSV's `reports` and `messages` columns are read as: `planner` and
    `local-repair` write the number of reports (every fact's `reports`) and the sum of those
    reports' hops; `flooding` writes `none` and the sum of every fact's `messages`."""
    if kind == "flooding":
        return None, sum(f.messages or 0 for f in facts)
    return sum(f.reports for f in facts), sum(h for f in facts for _, _, h in f.report_rows)


# ---------------------------------------------------------------------------------------
# After recovery (4.7, second list): reachability and the largest piece.
# ---------------------------------------------------------------------------------------
@dataclass
class Recovery:
    reachability: float
    extra_ms: list[float]
    largest_overall: float
    largest_per_shell: dict[int, float]


def after_recovery(model: Model, t: float = 0.0) -> Recovery:
    """One instant, every actor knowing everything: the true network at t."""
    world = model.world
    k = model.s.step_of(t)
    view = model.truth_view(t)
    working = [x for x in range(world.n) if x not in view.down_sats]
    up = [p for p in view.present if world.laser_delay_s(p, k) is not None]
    uf = UnionFind(working)
    for a, b in up:
        uf.union(a, b)
    shell_of = {x: world.shell_of_plane[p] for p, plane in enumerate(world.planes) for x in plane}
    per_shell = {}
    for shell in sorted(set(world.shell_of_plane)):
        members = [x for x in working if shell_of[x] == shell]
        sub = UnionFind(members)
        for a, b in up:
            if shell_of[a] == shell and shell_of[b] == shell:
                sub.union(a, b)
        per_shell[shell] = (sub.pieces()[0] / len(members)) if members else 0.0
    overall = uf.pieces()[0] / len(working) if working else 0.0
    reachable = 0
    extra = []
    k0, k1 = k, k + 1
    failed_graph, _ = model.graph(k0, k1, view)
    clean_graph, _ = model.graph(k0, k1, model.baseline_view())
    for d in model.directions:
        a = {uf.find(x) for x in world.ground_s(d.src, k) if x not in view.down_sats}
        b = {uf.find(x) for x in world.ground_s(d.dst, k) if x not in view.down_sats}
        if a & b:
            reachable += 1
            got = dijkstra(failed_graph, model.send(d.src)).delay.get(model.recv(d.dst))
            clean = dijkstra(clean_graph, model.send(d.src)).delay.get(model.recv(d.dst))
            if got is not None and clean is not None:
                extra.append((got - clean) * 1e3)
    share = reachable / len(model.directions) if model.directions else 0.0
    return Recovery(share, extra, overall, per_shell)


# ---------------------------------------------------------------------------------------
# Writing the model's results in the program's formats (4.10 Details), for comparison.
# ---------------------------------------------------------------------------------------
EVENT_COLUMNS = (
    "start_s", "end_s", "from", "to", "role", "pattern", "model", "strategy", "reaction",
    "state", "drop_cause", "failed_element", "one_way_ms", "route", "serving_norad_cat_id",
    "gateway",
)  # fmt: skip
FACT_COLUMNS = (
    "fact", "subject", "state", "t_s", "old", "detectors", "planner_known_s", "reports",
    "report_hops", "flood_nodes", "flood_last_s", "messages",
)  # fmt: skip
FLOOD_COLUMNS = ("fact", "node", "learn_s")


def route_text(route: tuple[int, ...], ids: list[int]) -> str:
    return " ".join(str(ids[x]) for x in route)


def fact_row(f: Fact, ids: list[int], flooding: bool) -> dict[str, str]:
    # 4.4 Details: an old fact has no detectors, no reports and no flood; the planner knows
    # it from the first step (0). As a fact nobody detects in the run, its flood columns are
    # 0 nodes, `never` and 0 messages (4.10 Details).
    detectors = ";".join(f"{node_text(n, ids)}@{instant_text(t)}" for n, t in f.detectors)
    flood = flooding
    return {
        "fact": str(f.number),
        "subject": element_text(f.subject, ids),
        "state": f.state,
        "t_s": repr(f.t_s),
        "old": "yes" if f.old else "no",
        "detectors": detectors,
        "planner_known_s": instant_text(f.planner_known_s),
        "reports": str(f.reports),
        "report_hops": "" if f.report_hops is None else str(f.report_hops),
        "flood_nodes": str(f.flood_nodes) if flood else "",
        "flood_last_s": instant_text(f.flood_last_s) if flood else "",  # type: ignore[arg-type]
        "messages": str(f.messages) if flood else "",
    }


# ---------------------------------------------------------------------------------------
# Reading the program's outputs and comparing (spec 4.10 Details, 6.3).
# ---------------------------------------------------------------------------------------
def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def close(a: float, b: float, tolerance: float) -> bool:
    if math.isinf(a) or math.isinf(b):
        return a == b
    return abs(a - b) <= tolerance


def compare_events(rows, lanes, ids, end_s) -> list[str]:
    problems = []
    got: dict[tuple[str, str, str, str], list[dict[str, str]]] = {}
    for r in rows:
        got.setdefault((r["from"], r["to"], r["strategy"], r["reaction"]), []).append(r)
    for key, lane in lanes.items():
        # SPEC-GAP: an interval of no length (a one-step run, or a change at the last step's
        # instant) is not written; read from the program, the spec does not say.
        want = [x for x in intervals_of(lane.samples, end_s) if x.end_s > x.start_s]
        have = got.pop(key, [])
        where = " ".join(key)
        if len(have) != len(want):
            problems.append(f"events {where}: {len(have)} intervals, model {len(want)}")
        for i, (r, x) in enumerate(zip(have, want, strict=False)):
            s = x.sample
            text = (r["state"], r["drop_cause"], r["failed_element"], r["route"])
            expected = (s.state, s.cause, s.element, route_text(s.route, ids))
            if text != expected:
                problems.append(f"events {where} #{i}: program {text}, model {expected}")
                break
            if not close(float(r["start_s"]), x.start_s, TIME_TOLERANCE_S) or not close(
                float(r["end_s"]), x.end_s, TIME_TOLERANCE_S
            ):
                problems.append(
                    f"events {where} #{i}: [{r['start_s']}, {r['end_s']}), model "
                    f"[{x.start_s!r}, {x.end_s!r})"
                )
                break
            if (r["one_way_ms"] == "") != (s.delay_s is None) or (
                s.delay_s is not None
                and abs(float(r["one_way_ms"]) - s.delay_s * 1e3) > DELAY_TOLERANCE_MS
            ):
                problems.append(f"events {where} #{i}: delay {r['one_way_ms']}, model {s.delay_s}")
                break
    for key in got:
        problems.append(f"events: program lane {key} has no model lane")
    return problems


def detectors_of(text: str) -> dict[str, float]:
    out = {}
    for part in filter(None, text.split(";")):
        node, _, when = part.rpartition("@")
        out[node] = parse_instant(when)
    return out


def compare_facts(rows, facts: list[Fact], ids, flooding: bool) -> tuple[list[str], dict]:
    """Facts are matched by (subject, state, instant) so that a different numbering of facts
    arising at one instant shows as one problem, not as every fact differing. Returns the
    program's fact number -> the model's fact."""
    problems = []
    model = {}
    for f in facts:
        model.setdefault((element_text(f.subject, ids), f.state), []).append(f)
    numbers = {}
    for r in rows:
        candidates = model.get((r["subject"], r["state"]), [])
        t = float(r["t_s"])
        match = next((f for f in candidates if close(f.t_s, t, TIME_TOLERANCE_S)), None)
        where = f"fact {r['fact']} {r['subject']} {r['state']} at {r['t_s']}"
        if match is None:
            problems.append(f"{where}: not in the model")
            continue
        candidates.remove(match)
        numbers[r["fact"]] = match
        want = fact_row(match, ids, flooding)
        if r["fact"] != want["fact"]:
            problems.append(f"{where}: numbered {r['fact']}, model {want['fact']}")
        for column in ("old", "reports", "report_hops", "flood_nodes", "messages"):
            if r[column] != want[column]:
                problems.append(f"{where}: {column} {r[column]!r}, model {want[column]!r}")
        for column in ("planner_known_s", "flood_last_s"):
            if (r[column] == "") != (want[column] == ""):
                problems.append(f"{where}: {column} {r[column]!r}, model {want[column]!r}")
            elif r[column] and not close(
                parse_instant(r[column]), parse_instant(want[column]), TIME_TOLERANCE_S
            ):
                problems.append(f"{where}: {column} {r[column]}, model {want[column]}")
        have, expected = detectors_of(r["detectors"]), detectors_of(want["detectors"])
        if set(have) != set(expected) or any(
            not close(have[x], expected[x], TIME_TOLERANCE_S) for x in have
        ):
            problems.append(f"{where}: detectors {r['detectors']!r}, model {want['detectors']!r}")
    for left in model.values():
        for f in left:
            problems.append(
                f"fact {element_text(f.subject, ids)} {f.state} at {f.t_s}: "
                "not in the program's facts"
            )
    return problems, numbers


def compare_floods(rows, numbers: dict, ids) -> list[str]:
    problems = []
    have: dict[str, dict[str, float]] = {}
    for r in rows:
        have.setdefault(r["fact"], {})[r["node"]] = float(r["learn_s"])
    for number, fact in numbers.items():
        if fact.old:
            continue
        want = {node_text(n, ids): t for n, t in fact.learn.items()}
        got = have.pop(number, {})
        if set(got) != set(want):
            problems.append(
                f"floods fact {number}: {len(got)} nodes, model {len(want)} "
                f"(e.g. {sorted(set(got) ^ set(want))[:3]})"
            )
            continue
        worst = max((abs(got[x] - want[x]) for x in got), default=0.0)
        if worst > TIME_TOLERANCE_S:
            problems.append(f"floods fact {number}: learning instants differ by {worst:.3e} s")
    for number in have:
        problems.append(f"floods: fact {number} is not a model fact")
    return problems


def compare_routes(rows, lanes, model: Model) -> list[str]:
    """Routes CSV (4.10 Details): one row per step, direction, strategy and reaction."""
    ids, settings = model.world.ids, model.s
    problems = []
    seen = 0
    start = None
    for r in rows:
        if start is None:
            start = st.parse_utc(r["utc"])
        k = round((st.parse_utc(r["utc"]).unix_ns - start.unix_ns) / (settings.step_s * 1e9))
        lane = lanes.get((r["from"], r["to"], r["strategy"], r["reaction"]))
        where = f"routes {r['utc']} {r['from']} -> {r['to']} {r['strategy']} {r['reaction']}"
        if lane is None:
            problems.append(f"{where}: no model lane")
            continue
        seen += 1
        t = settings.instant(k)
        sample = next(x for u, x in reversed(lane.samples) if u <= t)
        want = (
            sample.state,
            sample.cause,
            sample.element,
            route_text(sample.route, ids) if sample.state == "delivered" else "",
            "ok" if sample.state == "delivered" else "drop",
        )
        got = (r["state"], r["drop_cause"], r["failed_element"], r["route"], r["status"])
        if got != want:
            problems.append(f"{where}: program {got}, model {want}")
        elif sample.delay_s is not None and (
            abs(float(r["one_way_ms"]) - sample.delay_s * 1e3) > DELAY_TOLERANCE_MS
        ):
            problems.append(f"{where}: one-way {r['one_way_ms']}, model {sample.delay_s * 1e3}")
        ideal_s = model.ideal_s(lane, k)
        if (r["ideal_ms"] == "") != (ideal_s is None) or (
            ideal_s is not None and abs(float(r["ideal_ms"]) - ideal_s * 1e3) > DELAY_TOLERANCE_MS
        ):
            problems.append(f"{where}: ideal {r['ideal_ms']!r}, model {ideal_s}")
        if r["plan"] != lane.plan_marks.get(k, "") and lane.kind != "flooding":
            problems.append(f"{where}: plan {r['plan']!r}, model {lane.plan_marks.get(k, '')!r}")
    expected = sum(1 for _ in lanes) * settings.steps
    if seen != expected:
        problems.append(f"routes: {seen} rows, model {expected}")
    return problems[:50]


def measure_text(value) -> str:
    return "none" if value is None else repr(value)


def compare_measures(rows, lanes, model: Model) -> list[str]:
    """Measures CSV (4.10 Details), every value to 1e-9 (s or ms)."""
    problems = []
    t_f = failure_instant(model.schedule.failures)
    seen = set()
    for r in rows:
        key = (r["from"], r["to"], r["strategy"], r["reaction"])
        if r["reaction"] in ("baseline", "reference"):
            # SPEC-GAP: whether the measures CSV has rows for the two extra lanes; ignored.
            continue
        where = f"measures {' '.join(key)}"
        if key not in lanes:
            problems.append(f"{where}: no model lane")
            continue
        seen.add(key)
        m = model_measures(lanes, *key, t_f, model.s)
        m.reports, m.messages = lane_messages(model.facts, r["reaction"])
        for column in MEASURE_COLUMNS:
            want, got = getattr(m, column), r[column]
            if (got == "none") != (want is None):
                problems.append(f"{where}: {column} {got}, model {measure_text(want)}")
            elif want is not None and abs(float(got) - want) > TIME_TOLERANCE_S:
                problems.append(f"{where}: {column} {got}, model {want!r}")
    for key in lanes:
        if key[3] not in ("baseline", "reference") and key not in seen:
            problems.append(f"measures: no row for {' '.join(key)}")
    return problems


@dataclass(frozen=True)
class NetworkRow:
    k: int
    scope: str
    working: int
    pieces: int
    largest: int
    share: float | None


def network_rows(model: Model) -> list[NetworkRow]:
    """Network CSV (4.10 Details): per step and scope (`all`, then each shell), pieces of the
    working satellites joined by the true network's laser links that carry traffic."""
    world, out = model.world, []
    shell_of = {x: world.shell_of_plane[p] for p, plane in enumerate(world.planes) for x in plane}
    for k in range(model.s.steps):
        t = model.s.instant(k)
        view = model.truth_view(t)
        working = [x for x in range(world.n) if x not in view.down_sats]
        up = [p for p in sorted(view.present) if world.laser_delay_s(p, k) is not None]
        scopes = [("all", working, up)]
        for shell, name in enumerate(world.shell_names):
            members = [x for x in working if shell_of[x] == shell]
            links = [p for p in up if shell_of[p[0]] == shell and shell_of[p[1]] == shell]
            scopes.append((name, members, links))
        for name, members, links in scopes:
            uf = UnionFind(members)
            for a, b in links:
                uf.union(a, b)
            sizes = uf.pieces()
            largest = sizes[0] if sizes else 0
            # SPEC-GAP: with no working satellite in a scope the share is written `none`.
            share = largest / len(members) if members else None
            out.append(NetworkRow(k, name, len(members), len(sizes), largest, share))
    return out


def compare_network(rows, model: Model) -> list[str]:
    want = {(x.k, x.scope): x for x in network_rows(model)}
    problems, start = [], None
    for r in rows:
        if start is None:
            start = st.parse_utc(r["utc"])
        k = round((st.parse_utc(r["utc"]).unix_ns - start.unix_ns) / (model.s.step_s * 1e9))
        x = want.pop((k, r["scope"]), None)
        where = f"network {r['utc']} {r['scope']}"
        if x is None:
            problems.append(f"{where}: no model row")
            continue
        got = (int(r["working_satellites"]), int(r["pieces"]), int(r["largest_piece"]))
        if got != (x.working, x.pieces, x.largest):
            problems.append(f"{where}: {got}, model {(x.working, x.pieces, x.largest)}")
        share = r["largest_piece_share"]
        if (share == "none") != (x.share is None) or (
            x.share is not None and abs(float(share) - x.share) > 1e-12
        ):
            problems.append(f"{where}: share {share}, model {x.share}")
    problems += [f"network: no program row for step {k} {scope}" for k, scope in want]
    return problems[:50]


def model_measures(lanes, src, dst, strategy, kind, t_f, settings: Settings) -> Measures:
    def iv(k: str) -> list[Interval]:
        return intervals_of(lanes[(src, dst, strategy, k)].samples, settings.end_s)

    def at_steps(k: str) -> list[Sample]:
        by_t = dict(lanes[(src, dst, strategy, k)].samples)
        return [by_t[settings.instant(j)] for j in range(settings.steps)]

    steps = (at_steps(kind), at_steps("baseline"))
    return measures(iv(kind), iv("baseline"), iv("reference"), t_f, settings, steps)


# ---------------------------------------------------------------------------------------
# The program.
# ---------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Case:
    name: str
    links: str  # "three" or "plus-grid"
    scenario: str  # route-satellite | route-laser | repoint-pair | old-random
    duration_s: int = 40
    failure_s: float = 7.0
    length_s: float | None = None
    repoint: bool = False
    acquisition_s: float = 30.0
    replan_on_report: bool = False
    planner: str = "whole-interval"
    directions: tuple[tuple[str, str], ...] = (("New York", "London"), ("Frankfurt", "Singapore"))

    def settings(self) -> Settings:
        return Settings(
            steps=self.duration_s + 1,
            planner=self.planner,
            repoint=self.repoint,
            acquisition_s=self.acquisition_s,
            replan_on_report=self.replan_on_report,
        )

    @property
    def links_file(self) -> str:
        return ideal.THREE_LINKS_FILE if self.links == "three" else ideal.PLUS_GRID_SIDES_FILE


CASES = [
    Case("satellite-three", "three", "route-satellite"),
    Case("laser-grid", "plus-grid", "route-laser"),
    Case("transient-three", "three", "route-laser", length_s=10.0, acquisition_s=5.0),
    Case("repoint-three", "three", "repoint-pair", repoint=True, acquisition_s=2.0),
    Case(
        "report-three",
        "three",
        "route-satellite",
        replan_on_report=True,
        planner="start",
        duration_s=25,
    ),
    # Old news only: one step, 3% of the satellites failed an hour before, re-pointing on.
    Case("after-recovery", "three", "old-random", duration_s=0, repoint=True, acquisition_s=30.0),
]
BY_HAND: list[Case] = []


def run_failures(
    exe: Path,
    snapshot: Path,
    ground: Path,
    cities: Path,
    case: Case,
    failures_file: Path,
    work: Path,
) -> tuple[str, dict[str, list[dict[str, str]]]]:
    """Runs the `failures` program (spec 4.10) with the `routing` options of a Phase 3 case
    and the new ones; returns its summary and its six CSVs."""
    s = case.settings()
    names = ("routes", "events", "facts", "floods", "measures", "network")
    out = {name: work / f"{case.name}-{name}.csv" for name in names}
    command = [
        str(exe),
        "--snapshot", str(snapshot),
        "--ground", str(ground),
        "--cities", str(cities),
        "--pattern", case.links,
        "--model", "best-case",
        "--shells-file", str(snapshot / ideal.SHELLS_FILE),
        "--links-file", str(snapshot / case.links_file),
        "--start", ideal.EPOCH[:19],
        "--duration-s", str(case.duration_s),
        "--step-s", repr(s.step_s),
        "--replan-s", str(s.replan_steps),
        "--planner", s.planner,
        "--strategies", ",".join(s.strategies),
        "--max-ground-range-km", repr(vr.HYPATIA_RANGE_KM),
        "--hop-delay-ms", repr(s.hop_delay_s * 1e3),
        "--directions", "forward",
        "--failures", str(failures_file),
        "--reactions", ",".join(s.reactions),
        "--replan-on-report", "on" if s.replan_on_report else "off",
        "--repoint", "on" if s.repoint else "off",
        "--detection-s", repr(s.detection_s),
        "--hop-processing-s", repr(s.hop_processing_s),
        "--compute-s", repr(s.compute_s),
        "--planner-processing-s", repr(s.planner_processing_s),
        "--acquisition-s", repr(s.acquisition_s),
        "--plan-time", ideal.EPOCH[:19],
        "--out", str(out["routes"]),
        "--events-out", str(out["events"]),
        "--facts-out", str(out["facts"]),
        "--floods-out", str(out["floods"]),
        "--measures-out", str(out["measures"]),
        "--network-out", str(out["network"]),
    ]  # fmt: skip
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(
            f"error: failures exited with {result.returncode}\n{result.stdout}{result.stderr}"
        )
    return result.stdout, {name: read_csv(path) for name, path in out.items()}


def choose_failures(case: Case, model: Model) -> list[Failure]:
    """The case's failures, chosen from the baseline route (as spec 5's scenarios)."""
    s = model.s
    d = model.directions[0]
    probe = Lane("baseline", d.src, d.dst, "lowest-delay")
    k = s.step_of(case.failure_s)
    model.plan(
        probe,
        d,
        s.instant((k // s.replan_steps) * s.replan_steps),
        model.baseline_view(),
        mark=False,
    )
    route = probe.route or []
    sats = route[1:-1]
    if not sats:
        raise SystemExit(f"error: {case.name}: the baseline has no route at {case.failure_s} s")
    if case.scenario == "route-satellite":
        return [Failure("satellite", (sats[len(sats) // 2],), "", case.failure_s, case.length_s)]
    if case.scenario == "route-laser":
        if len(sats) < 2:
            raise SystemExit(f"error: {case.name}: the route has no laser hop")
        i = (len(sats) - 1) // 2
        return [Failure("laser_link", (sats[i], sats[i + 1]), "", case.failure_s, case.length_s)]
    if case.scenario == "repoint-pair":
        # The middle satellite x, and a satellite y of its cross-plane partner b's plane whose
        # own partner e is in x's plane: b and e then both have a free laser, facing each
        # other. y is the one whose e is nearest to b among those whose link e-b may be made
        # at the matching step (the first step after the detection).
        x = sats[len(sats) // 2]
        world = model.world
        partner = {}
        for p in world.planned:
            if world.plane_of(p.a) != world.plane_of(p.b):
                partner[p.a], partner[p.b] = p.b, p.a
        b = partner[x]
        k = s.first_step_at_or_after(case.failure_s + s.detection_s)
        best = None
        for y in world.planes[world.plane_of(b)]:
            e = partner.get(y)
            if y == b or e is None or world.plane_of(e) != world.plane_of(x) or e == x:
                continue
            if world.repair_ok(pair_of(e, b), k):
                key = (world.distance_km(pair_of(e, b), k), y)
                best = key if best is None or key < best else best
        if best is None:
            raise SystemExit(f"error: {case.name}: no satellite frees a laser facing {b}")
        return [Failure("satellite", (z,), "", case.failure_s, case.length_s) for z in (x, best[1])]
    if case.scenario == "old-random":
        rng = random.Random(0)
        chosen = rng.sample(range(model.n), max(1, model.n * 3 // 100))
        return [Failure("satellite", (x,), "", -3600.0, None) for x in sorted(chosen)]
    raise SystemExit(f"error: unknown scenario {case.scenario}")


def describe(case: Case, model: Model, lanes) -> None:
    s = model.s
    t_f = failure_instant(model.schedule.failures)
    print(f"  facts {len(model.facts)}, repair links {len(model.timeline.repairs)}")
    for f in model.facts[:12]:
        row = fact_row(f, model.world.ids, model.flooding)
        print(
            f"    {row['fact']} {row['subject']} {row['state']} t {row['t_s']} detectors "
            f"{len(f.detectors)} planner {row['planner_known_s']} reports {row['reports']} "
            f"flood {row['flood_nodes']} messages {row['messages']}"
        )
    for (src, dst, strategy, kind), lane in lanes.items():
        if kind in ("baseline", "reference"):
            continue
        m = model_measures(lanes, src, dst, strategy, kind, t_f, s)
        n = len(intervals_of(lane.samples, s.end_s))
        print(
            f"    {src} -> {dst} {strategy} {kind}: {n} intervals, black hole "
            f"{m.black_hole_s} s, loop {m.loop_s} s, restore {m.restore_s}, settle "
            f"{m.settle_s}, lost {m.lost_s} s, extra max {m.extra_max_ms} ms"
        )


def prepare(case: Case, work: Path, cities_file: Path, ground: Path):
    """The world, settings, directions and the case's failures (written to `work`)."""
    snapshot = work / "ideal-shell"
    if not (snapshot / ideal.THREE_LINKS_FILE).exists():
        ideal.write_ideal_shell(snapshot, st.DEFAULT_SNAPSHOT / st.EOP_FILE, ideal.HYPATIA)
        ideal.write_four_column_links(snapshot, ideal.HYPATIA)
    data = json.loads(cities_file.read_text(encoding="utf-8"))
    names = {c for pair in case.directions for c in pair}
    records = [c for c in data["cities"] if c["name"] in names]
    roles = {(p["from"], p["to"]): p["role"] for p in data["pairs"]}
    pairs = [{"from": a, "to": b, "role": roles.get((a, b), "study")} for a, b in case.directions]
    cities_path = work / f"cities-{case.name}.json"
    cities_path.write_text(json.dumps({"cities": records, "pairs": pairs}), encoding="utf-8")
    sites = [Site(c["name"], vr.City(c["name"], c["lat"], c["lon"])) for c in records]
    s = case.settings()
    start = st.parse_utc(ideal.EPOCH)
    lookahead = s.replan_steps
    times = [start.plus_ns(k * st.NS_PER_S) for k in range(s.steps + lookahead)]
    sided = read_sided_links(snapshot / case.links_file)
    world = IdealWorld(
        ideal.HYPATIA, sided, case.links, sites, load_gateways(ground), times, vr.HYPATIA_RANGE_KM
    )
    directions = [Direction(a, b, roles.get((a, b), "study")) for a, b in case.directions]
    probe = Model(world, s, [], directions)
    failures = choose_failures(case, probe)
    failures_path = work / f"failures-{case.name}.csv"
    write_failure_file(failures_path, failures, world.ids, start)
    planned = {pair_of(p.a, p.b) for p in world.planned}
    reread = parse_failure_file(
        failures_path.read_text(encoding="utf-8"),
        world.ids,
        planned,
        world.gateways,
        start,
        st.NS_PER_S,
    )
    if reread != [
        Failure(f.kind, f.sats, f.gateway, f.start_s, f.length_s, line=x.line)
        for f, x in zip(failures, reread, strict=True)
    ]:
        raise SystemExit(f"error: {case.name}: the failure file does not read back")
    return snapshot, cities_path, failures_path, world, s, directions, reread


def run_case(case: Case, exe: Path | None, args, work: Path) -> list[str]:
    snapshot, cities_path, failures_path, world, s, directions, failures = prepare(
        case, work, args.cities, args.ground
    )
    print(
        f"\n{case.name}: {case.links}, {case.scenario}, {len(failures)} failures, "
        f"{case.duration_s} s, {s.planner}, repoint {'on' if s.repoint else 'off'}"
    )
    model = Model(world, s, failures, directions)
    lanes = model.run()
    describe(case, model, lanes)
    if case.duration_s == 0:
        r = after_recovery(model)
        print(
            f"  after recovery: reachability {r.reachability:.3f}, largest piece "
            f"{r.largest_overall:.4f}, extra delay {[round(x, 3) for x in r.extra_ms]} ms"
        )
    if exe is None:
        return []
    _, out = run_failures(exe, snapshot, args.ground, cities_path, case, failures_path, work)
    counts = {name: len(rows) for name, rows in out.items()}
    print(f"  program rows compared: {counts}")
    expected = {
        "events": s.end_s > 0.0,
        "floods": any(f.flood_nodes for f in model.facts),
    }
    empty = [name for name, n in counts.items() if n == 0 and expected.get(name, True)]
    problems = [f"the program wrote no {name} rows" for name in empty]
    problems += compare_events(out["events"], lanes, world.ids, s.end_s)
    fact_problems, numbers = compare_facts(out["facts"], model.facts, world.ids, model.flooding)
    problems += fact_problems
    problems += compare_floods(out["floods"], numbers, world.ids)
    problems += compare_routes(out["routes"], lanes, model)
    problems += compare_measures(out["measures"], lanes, model)
    problems += compare_network(out["network"], model)
    return [f"{case.name}: {p}" for p in problems]


# ---------------------------------------------------------------------------------------
# Self-tests (no C++ program).
# ---------------------------------------------------------------------------------------
class SelfTestError(AssertionError):
    pass


def check(condition: bool, what: str) -> None:
    if not condition:
        raise SelfTestError(what)


def near(a: float, b: float) -> bool:
    return close(a, b, 1e-12)


def toy_diamond(**kwargs) -> ToyWorld:
    """Satellites 0 to 3 in one plane-free diamond: A sees 0, B sees 3, gateway G sees 0 and 2.
    Lasers 0-1 and 1-3 (10 ms each), 0-2 and 2-3 (15 ms each); ground links 1 ms."""
    planned = [
        PlannedLink(0, 1, "ahead", "behind"),
        PlannedLink(1, 3, "ahead", "behind"),
        PlannedLink(0, 2, "east", "west"),
        PlannedLink(2, 3, "east", "west"),
    ]
    return ToyWorld(
        planes=[[0, 1, 3], [2]],
        lasers=[("ahead", "behind", "east", "west")] * 4,
        planned=planned,
        laser_ms={(0, 1): 10.0, (1, 3): 10.0, (0, 2): 15.0, (2, 3): 15.0},
        ground_ms={"A": {0: 1.0}, "B": {3: 1.0}, "G": {0: 1.0, 2: 1.0}},
        cities=["A", "B"],
        gateways=["G"],
        **kwargs,
    )


def toy_settings(**kwargs) -> Settings:
    base = {"steps": 13, "replan_steps": 5, "planner": "start", "acquisition_s": 2.0}
    base.update(kwargs)
    return Settings(**base)  # type: ignore[arg-type]


def lane_intervals(model: Model, kind: str, strategy: str = "lowest-delay"):
    lanes = model.run()
    lane = lanes[("A", "B", strategy, kind)]
    return [
        (
            x.start_s,
            x.end_s,
            x.sample.state,
            x.sample.element,
            x.sample.route,
            None if x.sample.delay_s is None else round(x.sample.delay_s * 1e3, 9),
        )
        for x in intervals_of(lane.samples, model.s.end_s)
    ]


def same_intervals(got, want) -> bool:
    if len(got) != len(want):
        return False
    for g, w in zip(got, want, strict=True):
        if not (near(g[0], w[0]) and near(g[1], w[1]) and g[2:5] == w[2:5]):
            return False
        if (g[5] is None) != (w[5] is None) or (g[5] is not None and abs(g[5] - w[5]) > 1e-9):
            return False
    return True


def fact_of(model: Model, subject: Subject, state: str) -> Fact:
    return next(f for f in model.facts if f.subject == subject and f.state == state)


def test_flood_against_single_sources() -> None:
    rng = random.Random(4)
    for _ in range(60):
        n = rng.randint(2, 30)
        adj: dict[int, list[tuple[int, float]]] = {}
        for a in range(n):
            for b in range(a + 1, n):
                if rng.random() < 0.15:
                    w = rng.choice([0.001, 0.002, 0.0035, rng.random() / 100])
                    adj.setdefault(a, []).append((b, w))
                    adj.setdefault(b, []).append((a, w))
        sources = {x: rng.choice([0.0, 0.1, rng.random()]) for x in rng.sample(range(n), 3)}
        learn, first = flood_times(adj, sources)
        singles = [flood_times(adj, {x: t})[0] for x, t in sources.items()]
        for v in range(n):
            want = min(x.get(v, INF) for x in singles)
            check(near(learn.get(v, INF), want), f"flood time of {v}: {learn.get(v)} vs {want}")
            if v in learn:
                check(
                    first[v] is None or any(u == first[v] for u, _ in adj.get(v, ())),
                    "first link is a link",
                )
        check(all(first[x] is None or learn[x] < sources[x] for x in sources), "sources")
        for v, t in learn.items():
            via = [learn[u] + w for u, w in adj.get(v, ()) if u in learn]
            if first[v] is not None:
                check(near(min(via), t), f"node {v} learns at its best neighbour's time + w")


def test_union_find_against_bfs() -> None:
    rng = random.Random(7)
    for _ in range(100):
        n = rng.randint(1, 40)
        edges = [(a, b) for a in range(n) for b in range(a + 1, n) if rng.random() < 2.0 / n]
        uf = UnionFind(range(n))
        for a, b in edges:
            uf.union(a, b)
        check(uf.pieces() == bfs_pieces(range(n), edges), "union-find pieces against BFS")


def test_dijkstra_tie_rule() -> None:
    # 0 -> 1 -> 3 and 0 -> 2 -> 3, equal delays and hops: the lower predecessor (1) wins.
    adj = {0: [(2, 1.0), (1, 1.0)], 1: [(3, 1.0)], 2: [(3, 1.0)]}
    check(path_to(dijkstra(adj, 0), 3) == [0, 1, 3], "tie to the lower predecessor")
    adj = {0: [(1, 1.0), (3, 2.0)], 1: [(3, 1.0)]}
    check(path_to(dijkstra(adj, 0), 3) == [0, 3], "tie to fewer hops")
    check(path_to(dijkstra(adj, 0, banned=[3]), 3) is None, "banned node")


def test_failure_file() -> None:
    ids = [100, 101, 102]
    start = st.parse_utc("2000-01-01T00:00:00")
    text = (
        "# c\n" + FAILURE_HEADER + "\n"
        "satellite,101,,,2000-01-01T00:00:05,\n"
        "laser_link,100,102,,1999-12-31T23:00:00,10.5\n"
    )
    got = parse_failure_file(text, ids, {(0, 2)}, ["New York, NY"], start, st.NS_PER_S)
    check(
        [(f.kind, f.sats, f.start_s, f.length_s) for f in got]
        == [("satellite", (1,), 5.0, None), ("laser_link", (0, 2), -3600.0, 10.5)],
        "parse",
    )
    for bad in (
        "satellite,101,,,2000-01-01T00:00:05.5,\n",  # not a step
        "satellite,999,,,2000-01-01T00:00:05,\n",  # unknown
        "laser_link,100,101,,2000-01-01T00:00:05,\n",  # not planned
        "satellite,101,,,2000-01-01T00:00:05,-1\n",  # negative
        'gateway,,,"Nowhere",2000-01-01T00:00:05,\n',  # unknown gateway
        "satellite,101,,,2000-01-01T00:00:05,10\nsatellite,101,,,2000-01-01T00:00:09,\n",
    ):
        try:
            parse_failure_file(FAILURE_HEADER + "\n" + bad, ids, {(0, 2)}, [], start, st.NS_PER_S)
        except FailureFileError:
            continue
        raise SelfTestError(f"failure file accepted {bad!r}")
    g = parse_failure_file(
        FAILURE_HEADER + '\ngateway,,,"New York, NY",2000-01-01T00:00:01,\n',
        ids,
        set(),
        ["New York, NY"],
        start,
        st.NS_PER_S,
    )
    check(g[0].gateway == "New York, NY", "quoted gateway")


def test_satellite_three_modes() -> None:
    """Satellite 1 fails at 2 s (written out by hand in the comments)."""
    world = toy_diamond()
    model = Model(
        world,
        toy_settings(),
        [Failure("satellite", (1,), "", 2.0, None)],
        [Direction("A", "B", "study")],
    )
    # Detection: 0 detects link 0-1 and 3 detects link 1-3 at 2.1 s; nobody sees satellite 1.
    f01 = fact_of(model, ("link", (0, 1)), "down")
    f13 = fact_of(model, ("link", (1, 3)), "down")
    check(f01.detectors == [(("satellite", 0), 2.1)], f"detectors of 0-1 {f01.detectors}")
    check(f13.detectors == [(("satellite", 3), 2.1)], f"detectors of 1-3 {f13.detectors}")
    check(fact_of(model, ("satellite", 1), "down").detectors == [], "satellite 1 unseen")
    # Reports: 0 -> G directly (1 ms + 1 ms per hop) = 2.102; 3 -> 2 -> G = 2.1 + 0.016 + 0.002.
    check(near(f01.planner_known_s, 2.102) and f01.report_hops == 1, "report of 0-1")
    check(near(f13.planner_known_s, 2.118) and f13.report_hops == 2, "report of 1-3")
    # Flood of 0-1 from 0 at 2.1: A and G at 2.102, 2 via G at 2.104, 3 at 2.120, B at 2.122.
    learn = {n[1]: t for n, t in f01.learn.items()}
    want = {0: 2.1, "A": 2.102, "G": 2.102, 2: 2.104, 3: 2.12, "B": 2.122}
    check(set(learn) == set(want) and all(near(learn[x], want[x]) for x in want), f"{learn}")
    # Messages: 0 sends 3; A 0; G 1; 2 sends 2; 3 sends 1; B 0.
    check(f01.messages == 7 and f13.messages == 7, f"messages {f01.messages} {f13.messages}")
    # Views: the planner knows both of 1's links down by 2.118, so it takes 1 as down; satellite
    # 0 knows only its own link at 2.2 (2.1 + compute), so 1 is not down in its view.
    check(1 in model.planner_view(2.118).down_sats, "every planned link down: satellite down")
    check(1 not in model.planner_view(2.117).down_sats, "one link known: satellite still up")
    zero = model.node_view(("satellite", 0), 2.2)
    check(1 not in zero.down_sats and (0, 1) not in zero.present, f"view of 0 {zero}")
    route = (0, 1, 3)
    good = ("delivered", "", route, 22.0)
    detour = ("delivered", "", (0, 2, 3), 32.0)
    hole = ("black_hole", "satellite:101", route, None)

    def iv(a, b, x):
        return (a, b, x[0], x[1], x[2], x[3])

    planner = [iv(0.0, 2.0, good), iv(2.0, 5.0, hole), iv(5.0, 12.0, detour)]
    check(same_intervals(lane_intervals(model, "planner"), planner), "planner intervals")
    check(same_intervals(lane_intervals(model, "reference"), planner), "reference intervals")
    check(same_intervals(lane_intervals(model, "baseline"), [iv(0.0, 12.0, good)]), "baseline")
    local = [iv(0.0, 2.0, good), iv(2.0, 2.1, hole), iv(2.1, 12.0, detour)]
    check(same_intervals(lane_intervals(model, "local-repair"), local), "local-repair intervals")
    flood_hole = ("black_hole", "satellite:101", (0,), None)
    flooding = [iv(0.0, 2.0, good), iv(2.0, 2.2, flood_hole), iv(2.2, 12.0, detour)]
    got = lane_intervals(model, "flooding")
    check(same_intervals(got, flooding), f"flooding intervals {got}")
    lanes = model.run()
    m = model_measures(lanes, "A", "B", "lowest-delay", "local-repair", 2.0, model.s)
    check(near(m.black_hole_s, 0.1) and near(m.restore_s, 0.1) and near(m.lost_s, 0.1), "m")
    check(near(m.settle_s, 3.0), f"local repair settles when the reference re-plans {m}")
    # Extra delay at steps 2 to 12 against the baseline's 22 ms: 10 ms from step 3 on (at
    # step 2 the repaired lane is a black hole).
    check(near(m.extra_min_ms, 10.0) and near(m.extra_max_ms, 10.0), f"extra {m}")
    check(near(m.extra_settled_ms, 10.0), f"extra once settled {m}")
    check(lane_messages(model.facts, "planner") == (2, 3), "reports 2, hops 1 + 2")
    check(lane_messages(model.facts, "flooding") == (None, 14), "flood messages 7 + 7")


def test_transient_link() -> None:
    """Link 0-1 fails at 2 s for 3.5 s; it acquires 2 s and carries from 7.5 s."""
    world = toy_diamond()
    model = Model(
        world,
        toy_settings(),
        [Failure("laser_link", (0, 1), "", 2.0, 3.5)],
        [Direction("A", "B", "study")],
    )
    down = fact_of(model, ("link", (0, 1)), "down")
    up = fact_of(model, ("link", (0, 1)), "up")
    check(down.detectors == [(("satellite", 0), 2.1), (("satellite", 1), 2.1)], "both ends")
    check(
        near(up.t_s, 7.5) and up.detectors == [(("satellite", 0), 7.5), (("satellite", 1), 7.5)],
        f"up fact {up}",
    )
    check(near(up.planner_known_s, 7.502), "the planner hears of the return from 0")
    good = ("delivered", "", (0, 1, 3), 22.0)
    detour = ("delivered", "", (0, 2, 3), 32.0)
    hole = ("black_hole", "link:100-101", (0, 1, 3), None)

    def iv(a, b, x):
        return (a, b, x[0], x[1], x[2], x[3])

    want = [iv(0.0, 2.0, good), iv(2.0, 5.0, hole), iv(5.0, 10.0, detour), iv(10.0, 12.0, good)]
    check(same_intervals(lane_intervals(model, "planner"), want), "planner with a return")
    keep = [iv(0.0, 2.0, good), iv(2.0, 5.0, hole), iv(5.0, 12.0, detour)]
    check(same_intervals(lane_intervals(model, "planner", "keep-until-broken"), keep), "keep")
    local = [iv(0.0, 2.0, good), iv(2.0, 2.1, hole), iv(2.1, 10.0, detour), iv(10.0, 12.0, good)]
    check(same_intervals(lane_intervals(model, "local-repair"), local), "local-repair return")
    flood_hole = ("black_hole", "link:100-101", (0,), None)
    flooding = [
        iv(0.0, 2.0, good),
        iv(2.0, 2.2, flood_hole),
        iv(2.2, 7.6, detour),
        iv(7.6, 12.0, good),
    ]
    got = lane_intervals(model, "flooding")
    check(same_intervals(got, flooding), f"flooding with a return {got}")


def test_deferred_detection() -> None:
    """Link 1-3's geometry is down at steps 0 to 3: satellite 1's failure at 2 s is detected
    by 3 at step 4 + 0.1 s; with the failure ending at 3.5 s it is never detected."""
    world = toy_diamond(down_steps={(1, 3): {0, 1, 2, 3}})
    model = Model(
        world,
        toy_settings(),
        [Failure("satellite", (1,), "", 2.0, None)],
        [Direction("A", "B", "study")],
    )
    f13 = fact_of(model, ("link", (1, 3)), "down")
    check(f13.detectors == [(("satellite", 3), 4.1)], f"deferred {f13.detectors}")
    model = Model(
        world,
        toy_settings(),
        [Failure("satellite", (1,), "", 2.0, 1.5)],
        [Direction("A", "B", "study")],
    )
    f13 = fact_of(model, ("link", (1, 3)), "down")
    check(f13.detectors == [(("satellite", 3), INF)], f"never {f13.detectors}")
    check(f13.planner_known_s == INF and f13.reports == 0, "a never-detected fact")


def toy_planes() -> ToyWorld:
    """Plane 0: satellites 0 to 3; plane 1: 4 to 9. `three` lasers; in-plane rings; plane 0's
    east lasers (0, 2) to 5 and 7, plane 1's east lasers (4, 6) to 1 and 3; 8 (east) and 9
    (west) never had a partner. Repairs allowed: 0-2 (across dead 1) and 2-9."""
    planes = [[0, 1, 2, 3], [4, 5, 6, 7, 8, 9]]
    lasers = []
    for plane in planes:
        for i, _ in enumerate(plane):
            lasers.append(("ahead", "behind", "east" if i % 2 == 0 else "west"))
    planned = []
    laser_ms = {}
    for plane in planes:
        for a, b in zip(plane, plane[1:] + plane[:1], strict=True):
            planned.append(PlannedLink(a, b, "ahead", "behind"))
            laser_ms[pair_of(a, b)] = 5.0
    for a, b in ((0, 5), (2, 7), (4, 1), (6, 3)):
        planned.append(PlannedLink(a, b, "east", "west"))
        laser_ms[pair_of(a, b)] = 7.0
    laser_ms[(0, 2)] = 9.0
    laser_ms[(2, 9)] = 8.0
    return ToyWorld(
        planes,
        lasers,
        planned,
        laser_ms,
        {"A": {0: 1.0}, "B": {3: 1.0}, "G": {4: 1.0}},
        ["A", "B"],
        ["G"],
        repairable={(0, 2), (2, 9)},
    )


def test_repointing() -> None:
    world = toy_planes()
    s = toy_settings(repoint=True)
    model = Model(world, s, [Failure("satellite", (1,), "", 2.0, 6.0)], [Direction("A", "B", "x")])
    # 0 (ahead) and 2 (behind) lose 1 and detect at 2.1: matched at step 3, carry at 5.
    check(
        [(r.a, r.b, r.side_a, r.side_b, r.matched_step, r.carry_s) for r in model.timeline.repairs]
        == [(0, 2, "ahead", "behind", 3, 5.0)],
        f"repairs {model.timeline.repairs}",
    )
    up = fact_of(model, ("link", (0, 2)), "up")
    check(near(up.t_s, 5.0) and len(up.detectors) == 2, "repair link up at 5 s")
    # Satellite 1 returns at 8: the repair is dropped at once; 0-1, 1-2 and 1-4 acquire to 10.
    drop = fact_of(model, ("link", (0, 2)), "down")
    check(
        near(drop.t_s, 8.0)
        and drop.detectors == [(("satellite", 0), 8.0), (("satellite", 2), 8.0)],
        "drop",
    )
    for p in ((0, 1), (1, 2), (1, 4)):
        check(near(fact_of(model, ("link", p), "up").t_s, 10.0), f"{p} back at 10 s")
    # The cross laser of 4 lost 1 too but nothing faces it: never matched.
    # A link failure 2-7 frees 2 (east) and 7 (west): 2 pairs with 9, which never had one.
    model = Model(
        world, s, [Failure("laser_link", (2, 7), "", 2.0, None)], [Direction("A", "B", "x")]
    )
    check(
        [(r.a, r.b, r.matched_step) for r in model.timeline.repairs] == [(2, 9, 3)],
        f"cross repair {model.timeline.repairs}",
    )
    # Old failure: matched at step 0 and in force from the first step (an hour old).
    model = Model(
        world, s, [Failure("satellite", (1,), "", -3600.0, None)], [Direction("A", "B", "x")]
    )
    check(
        [(r.a, r.b, r.matched_step, r.carry_s) for r in model.timeline.repairs] == [(0, 2, 0, 0.0)],
        f"old repair {model.timeline.repairs}",
    )
    old = fact_of(model, ("link", (0, 2)), "up")
    check(old.old, "old repair known from the first step")
    lanes = model.run()
    first = lanes[("A", "B", "lowest-delay", "planner")].samples[0][1]
    # The ring's own 3-0 link is the shortest; the old news is in every view from the start.
    check(first.state == "delivered" and first.route == (0, 3), f"old news route {first}")
    view = model.planner_view(0.0)
    check(1 in view.down_sats and (0, 2) in view.present, f"old view {view}")


def test_after_recovery() -> None:
    world = toy_planes()
    model = Model(
        world,
        toy_settings(steps=1),
        [Failure("satellite", (1,), "", -10.0, None), Failure("satellite", (3,), "", -10.0, None)],
        [Direction("A", "B", "x")],
    )
    r = after_recovery(model)
    # Working: 0, 2, 4..9; lasers 0-5, 2-7 and plane 1's ring: one piece of 8; B sees only 3.
    check(near(r.largest_overall, 1.0) and r.reachability == 0.0, f"recovery {r}")


def test_ideal_baseline_against_phase3(work: Path) -> None:
    """On the ideal shell's +Grid with no failures, the baseline equals validate_routing's
    model of Phase 3 at every step (lowest-delay, start planner, every step)."""
    case = Case("selftest", "plus-grid", "route-satellite", duration_s=3)
    snapshot = work / "ideal-shell"
    ideal.write_ideal_shell(snapshot, st.DEFAULT_SNAPSHOT / st.EOP_FILE, ideal.HYPATIA)
    ideal.write_four_column_links(snapshot, ideal.HYPATIA)
    data = json.loads((st.REPO_ROOT / "data/ground/cities.json").read_text(encoding="utf-8"))
    names = {c for pair in case.directions for c in pair}
    records = [c for c in data["cities"] if c["name"] in names]
    sites = [Site(c["name"], vr.City(c["name"], c["lat"], c["lon"])) for c in records]
    start = st.parse_utc(ideal.EPOCH)
    times = [start.plus_ns(k * st.NS_PER_S) for k in range(6)]
    world = IdealWorld(
        ideal.HYPATIA,
        read_sided_links(snapshot / ideal.PLUS_GRID_SIDES_FILE),
        "plus-grid",
        sites,
        [],
        times,
        vr.HYPATIA_RANGE_KM,
    )
    s = Settings(
        steps=4,
        replan_steps=1,
        planner="start",
        strategies=("lowest-delay",),
        reactions=("flooding",),
    )
    directions = [Direction(a, b, "study") for a, b in case.directions]
    lanes = Model(world, s, [], directions).run()
    vcase = vr.Case(
        "x",
        ideal.HYPATIA,
        3 * st.NS_PER_S,
        st.NS_PER_S,
        st.NS_PER_S,
        ("lowest-delay",),
        max_range_km=vr.HYPATIA_RANGE_KM,
        forward_only=True,
    )
    cities = {c.name: c.city for c in sites}
    links = np.array([[p.a, p.b] for p in world.planned])
    rows = vr.Model(vcase, cities, list(case.directions), world.positions, links).run(4)
    for (src, dst, _, kind), lane in lanes.items():
        for k in range(4):
            sample = next(x for u, x in reversed(lane.samples) if u <= k)
            row = rows[(k, src, dst, "lowest-delay")]
            check(sample.state == "delivered" and row.status == "ok", f"{kind} {k} delivered")
            check(
                abs(sample.delay_s * 1e3 - row.one_way_ms) < 1e-9,  # type: ignore[operator]
                f"{kind} {src}->{dst} {k}: {sample.delay_s} vs {row.one_way_ms}",
            )
            if kind != "flooding":
                check(sample.route == row.route, f"{kind} route at {k}")


def test_drop_cause_order() -> None:
    """4.6 Details: a failed element anywhere on the held route comes first, and a down
    satellite is `failed_satellite` even when the link to it has failed too."""
    world = toy_diamond(down_steps={(0, 1): {3, 4}})
    failures = [
        Failure("laser_link", (1, 3), "", 3.0, None),
        Failure("satellite", (3,), "", 3.0, None),
    ]
    model = Model(world, toy_settings(), failures, [Direction("A", "B", "study")])
    got = lane_intervals(model, "planner")
    # At 3 s the hop 0-1 is out of sight (a Phase 3 cause) and 3 is down further on: the
    # failure wins. The planner hears only of 2-3 (1 is cut off, B reaches no gateway), so its
    # plans keep 0-1-3: a black hole to the end.
    check(got[1][:4] == (3.0, 12.0, "black_hole", "satellite:103"), f"cause order {got}")


def test_repaired_route_kept() -> None:
    """With no gateway in reach the planner never hears of satellite 1: its plans still have
    the dead hop 0 -> 1, so local repair keeps its repaired route, recorded `kept`."""
    world = toy_diamond()
    world.ground_ms["G"] = {}
    model = Model(
        world,
        toy_settings(),
        [Failure("satellite", (1,), "", 2.0, None)],
        [Direction("A", "B", "study")],
    )
    lanes = model.run()
    lane = lanes[("A", "B", "lowest-delay", "local-repair")]
    check(lane.plan_marks == {0: "replaced", 5: "kept", 10: "kept"}, f"{lane.plan_marks}")
    got = [(x.start_s, x.sample.state, x.sample.route) for x in intervals_of(lane.samples, 12.0)]
    check(
        got
        == [
            (0.0, "delivered", (0, 1, 3)),
            (2.0, "black_hole", (0, 1, 3)),
            (2.1, "delivered", (0, 2, 3)),
        ],
        f"kept repair {got}",
    )
    planner = lanes[("A", "B", "lowest-delay", "planner")]
    check(intervals_of(planner.samples, 12.0)[-1].sample.state == "black_hole", "never hears")


def test_old_facts_and_no_measures() -> None:
    """An old failure gives its element's fact and one per link, all old; with no fresh failure
    the measures counting from t_f are none."""
    world = toy_diamond()
    model = Model(
        world,
        toy_settings(),
        [Failure("satellite", (1,), "", -60.0, None)],
        [Direction("A", "B", "study")],
    )
    facts = [(f.subject, f.state, f.old, f.detectors, f.planner_known_s) for f in model.facts]
    check(
        facts
        == [
            (("satellite", 1), "down", True, [], 0.0),
            (("link", (0, 1)), "down", True, [], 0.0),
            (("link", (1, 3)), "down", True, [], 0.0),
        ],
        f"old facts {facts}",
    )
    lanes = model.run()
    m = model_measures(lanes, "A", "B", "lowest-delay", "planner", None, model.s)
    check(m.failure_s is None and m.black_hole_s is None and m.settle_s is None, f"{m}")
    ignored = Model(
        world,
        toy_settings(),
        [Failure("satellite", (1,), "", -60.0, 60.0)],
        [Direction("A", "B", "study")],
    )
    check(ignored.facts == [], "a failure that ended at the start is ignored")


def test_satellite_down_inference() -> None:
    """4.4: a working satellite whose neighbours all fail is not taken as down (it detects
    the losses itself); a dead one whose neighbours report is."""
    world = toy_diamond()
    # 0's neighbours 1 and 2 fail: 0 detects both link facts, so 0 stays up in every view.
    model = Model(
        world,
        toy_settings(),
        [Failure("satellite", (1,), "", 2.0, None), Failure("satellite", (2,), "", 2.0, None)],
        [Direction("A", "B", "study")],
    )
    view = model.planner_view(10.0)
    check(fact_of(model, ("link", (0, 1)), "down").planner_known_s < 10.0, "0's report")
    check(0 not in view.down_sats, f"a working satellite cut off is still up {view}")
    # 1 fails: its links' facts are detected by 0 and 3 only, so 1 is taken as down.
    model = Model(
        world,
        toy_settings(),
        [Failure("satellite", (1,), "", 2.0, None)],
        [Direction("A", "B", "study")],
    )
    check(1 in model.planner_view(10.0).down_sats, "a dead satellite whose neighbours report")
    # Old link facts never make the guess: old 0-1 and 1-3 link failures leave 1 up.
    model = Model(
        world,
        toy_settings(),
        [
            Failure("laser_link", (0, 1), "", -9.0, None),
            Failure("laser_link", (1, 3), "", -9.0, None),
        ],
        [Direction("A", "B", "study")],
    )
    check(1 not in model.planner_view(0.0).down_sats, "old link facts do not take 1 down")


def test_network_rows() -> None:
    world = toy_planes()
    model = Model(
        world,
        toy_settings(steps=2),
        [Failure("satellite", (1,), "", -60.0, None), Failure("laser_link", (4, 5), "", 1.0, None)],
        [Direction("A", "B", "x")],
    )
    rows = [(x.k, x.scope, x.working, x.pieces, x.largest) for x in network_rows(model)]
    # Step 0: 9 working, all in one piece (0-3 joins plane 0, 0-5 and 2-7 join plane 1).
    # Step 1: losing 4-5 leaves plane 1's ring as a path: still one piece.
    check(
        rows
        == [
            (0, "all", 9, 1, 9),
            (0, "toy-s1", 9, 1, 9),
            (1, "all", 9, 1, 9),
            (1, "toy-s1", 9, 1, 9),
        ],
        f"network {rows}",
    )


SELF_TESTS = [
    test_drop_cause_order,
    test_repaired_route_kept,
    test_old_facts_and_no_measures,
    test_network_rows,
    test_satellite_down_inference,
    test_dijkstra_tie_rule,
    test_flood_against_single_sources,
    test_union_find_against_bfs,
    test_failure_file,
    test_satellite_three_modes,
    test_transient_link,
    test_deferred_detection,
    test_repointing,
    test_after_recovery,
]


def self_test() -> int:
    failed = 0
    for test in [*SELF_TESTS, test_ideal_baseline_against_phase3]:
        try:
            if test is test_ideal_baseline_against_phase3:
                with tempfile.TemporaryDirectory(prefix="satsim-failures-selftest-") as work:
                    test(Path(work))
            else:
                test()
            print(f"  ok   {test.__name__}")
        except SelfTestError as e:
            failed += 1
            print(f"  FAIL {test.__name__}: {e}")
    if failed:
        print(f"\nFAIL: {failed} self-tests", file=sys.stderr)
        return 1
    print("\nPASS: every self-test")
    return 0


# ---------------------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    st.forbid_network()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="run the self-tests only")
    parser.add_argument("--failures-program", type=Path, help="the C++ failures program")
    parser.add_argument(
        "--model-only",
        action="store_true",
        help="run the cases' model without the program (to inspect them)",
    )
    parser.add_argument("--cities", type=Path, default=st.REPO_ROOT / "data/ground/cities.json")
    parser.add_argument("--ground", type=Path, default=st.REPO_ROOT / "data/ground/2026-11-19")
    parser.add_argument(
        "--cases",
        default=",".join(c.name for c in CASES),
        help=f"comma-separated, of {', '.join(c.name for c in CASES + BY_HAND)}",
    )
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    exe = args.failures_program
    if not args.model_only and (exe is None or not exe.is_file()):
        print(
            f"error: --failures-program {exe} not found (it is needed without --self-test "
            "or --model-only)",
            file=sys.stderr,
        )
        return 2
    chosen = args.cases.split(",")
    unknown = set(chosen) - {c.name for c in CASES + BY_HAND}
    if unknown:
        print(f"error: unknown cases {sorted(unknown)}", file=sys.stderr)
        return 2
    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="satsim-validate-failures-") as work_dir:
        for case in CASES + BY_HAND:
            if case.name in chosen:
                problems += run_case(case, None if args.model_only else exe, args, Path(work_dir))
    if problems:
        print(f"\nFAIL: {len(problems)} problems", file=sys.stderr)
        for line in problems[:60]:
            print(f"  {line}", file=sys.stderr)
        return 1
    print("\nPASS" if not args.model_only else "\nmodel only: nothing compared")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

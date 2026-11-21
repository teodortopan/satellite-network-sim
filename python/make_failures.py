#!/usr/bin/env python3
"""Write the failure files of the Phase 4 runs (docs/phase-4-failures.md 4.2 and section 5).

A failure file is the `failures` program's input (spec 4.2): comment lines starting with `#`,
the header `kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s`, then one failure
per line with the unused fields empty. `start_utc` is written as the programs take `--start`
(YYYY-MM-DDTHH:MM:SS[.f], UTC); `length_s` is empty for a permanent failure. A gateway's name
is written in double quotes when it holds a comma or a quote, a quote inside it doubled. A
laser link is written with the lower catalogue number first. Every file starts with comment
lines saying what it is: the scenario or draw, the direction, the instant, the seed.

Three kinds of file, used by python/run_phase4.py:

- Hand-picked (spec 5, first table), for one direction at one failure instant T, from the
  BASELINE route of that direction at T: the `baseline` row of the `failures` program's
  routes CSV at T, from a run with no failure file and the main settings (run_phase4.py makes
  that run). The route's satellites are its `route` column in order, from the sending end.
    route-satellite   the middle satellite: index len // 2 of the route's satellites;
    access-satellite  the first satellite;
    route-laser       the laser link between satellites len // 2 - 1 and len // 2; left out
                      (skipped) when the route has fewer than two satellites;
    plane             every satellite in the middle satellite's plane (shell and plane from
                      the `constellation` program's CSV at the plan time);
    region            every satellite in a plane, any shell, whose sub-satellite point is
                      within 1,000 km great-circle distance (run_phase3.great_circle_km, on
                      the mean sphere) of the middle satellite's at T; the points are the
                      geodetic latitude and longitude of the `propagate` program at T;
    gateway           the gateway the route uses (the realistic model only).
  Each is permanent, or transient with a length. A direction with no delivered baseline route
  at T has no scenario: it is skipped, with the reason.
- Random shares (spec 5, "After recovery"): a share of the satellites in planes (from the
  `constellation` CSV), or of the planned laser links (a `--links-out` file), drawn with
  numpy.random.default_rng(seed).choice on the sorted list (satellites by catalogue number;
  links as (lower, higher) pairs, sorted), without replacement, count = round(share x n)
  (Python's round), every failure permanent and dated one hour before the run's one step, so
  that it is old news (spec 4.2).
- Attack: the same shares of the satellites in planes, taken in order of the number of the
  given baseline routes that pass through each satellite (most first; ties to the lower
  catalogue number), dated one hour before the step.

Command line, for use by hand (the runner calls the functions):

    python3 python/make_failures.py random-satellites --constellation-csv C --share 0.01 \\
        --seed 0 --step-utc 2026-11-18T21:20:00 --out f.csv
    python3 python/make_failures.py random-lasers --links-file L --share 0.01 --seed 0 \\
        --step-utc 2026-11-18T21:20:00 --out f.csv
    python3 python/make_failures.py hand-picked --scenario plane --routes-csv R \\
        --from "New York" --to London --at 2026-11-18T21:20:00 --constellation-csv C \\
        [--propagate build/validate/bin/propagate] [--length-s 10] --out f.csv
    python3 python/make_failures.py attack --constellation-csv C --routes-csv R \\
        --at 2026-11-18T21:20:00 --share 0.01 --out f.csv

The routes CSV of `hand-picked` and `attack` is a `failures` (or `routing`) routes CSV that
holds the baseline rows at --at (rows without a `reaction` column count as baseline).
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

import numpy as np

import snapshot_tools as st
from run_phase3 import great_circle_km

HEADER = "kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s"
SCENARIOS = (
    "route-satellite",
    "access-satellite",
    "route-laser",
    "plane",
    "region",
    "gateway",
)
REGION_RADIUS_KM = 1000.0
OLD_NEWS_S = 3600  # Random and attack failures are dated this long before the step.


# ---------------------------------------------------------------------------------------
# The file.
# ---------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Failure:
    """One line of a failure file."""

    kind: str  # satellite, laser_link or gateway
    start_utc: str
    a: int | None = None
    b: int | None = None
    gateway: str | None = None
    length_s: float | None = None  # None: permanent


def gateway_field(name: str) -> str:
    """Spec 4.2: quoted when it holds a comma (or a quote), a quote inside it doubled."""
    if "," in name or '"' in name:
        return '"' + name.replace('"', '""') + '"'
    return name


def length_text(length_s: float | None) -> str:
    if length_s is None:
        return ""
    return str(int(length_s)) if float(length_s).is_integer() else repr(float(length_s))


def failure_line(f: Failure) -> str:
    if f.kind == "satellite":
        fields = [str(f.a), "", ""]
    elif f.kind == "laser_link":
        a, b = sorted((f.a, f.b))
        fields = [str(a), str(b), ""]
    elif f.kind == "gateway":
        fields = ["", "", gateway_field(f.gateway)]
    else:
        raise ValueError(f"unknown failure kind {f.kind!r}")
    return ",".join([f.kind, *fields, f.start_utc, length_text(f.length_s)])


def write_failure_file(path: Path, failures: list[Failure], comments: list[str]) -> None:
    lines = [f"# {c}" for c in comments] + [HEADER] + [failure_line(f) for f in failures]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def satellite_failures(ids: Iterable[int], start_utc: str, length_s=None) -> list[Failure]:
    return [Failure("satellite", start_utc, a=i, length_s=length_s) for i in sorted(ids)]


def shifted_utc(utc: str, seconds: int) -> str:
    return st.parse_utc(utc).plus_ns(seconds * st.NS_PER_S).iso()


# ---------------------------------------------------------------------------------------
# Inputs.
# ---------------------------------------------------------------------------------------
def read_planes(constellation_csv: Path) -> dict[int, tuple[str, int, int]]:
    """Satellites in planes -> (shell, plane index, index in plane), from the CSV of the
    `constellation` program."""
    planes = {}
    with constellation_csv.open(newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["plane_index"]:
                planes[int(r["norad_cat_id"])] = (
                    r["shell"],
                    int(r["plane_index"]),
                    int(r["index_in_plane"]),
                )
    return planes


def read_links(links_file: Path) -> list[tuple[int, int]]:
    """The planned laser links of a link file (`--links-out`), each (lower, higher), sorted."""
    links = []
    header = None
    with links_file.open(encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if header is None:
                header = line
                continue
            a, b = (int(x) for x in line.split(",")[:2])
            links.append((min(a, b), max(a, b)))
    if len(set(links)) != len(links):
        raise SystemExit(f"error: {links_file} holds a link twice")
    return sorted(links)


def subsatellite_points(
    propagate: Path, snapshot: Path, at_utc: str, ids: list[int], out_csv: Path
) -> dict[int, tuple[float, float]]:
    """Catalogue number -> geodetic (latitude, longitude) in degrees at one instant."""
    st.run_propagate(propagate, snapshot, st.parse_utc(at_utc), 0, st.NS_PER_S, ids, out_csv)
    points = {
        r["norad_cat_id"]: (r["geodetic_latitude_deg"], r["geodetic_longitude_deg"])
        for r in st.read_propagate_csv(out_csv)
    }
    out_csv.unlink()
    missing = set(ids) - set(points)
    if missing:
        raise SystemExit(f"error: propagate gave no position for {len(missing)} satellites")
    return points


@dataclass(frozen=True)
class BaselineRoute:
    """A direction's baseline at one instant: delivered or not, and its route."""

    state: str
    drop_cause: str
    satellites: tuple[int, ...]
    gateway: str  # "" in the best case
    gateway_source: str
    one_way_ms: float | None


def baseline_routes(rows: Iterable[dict], at_utc: str) -> dict[tuple[str, str], BaselineRoute]:
    """(from, to) -> its baseline at at_utc, from routes-CSV rows (one strategy expected)."""
    found = {}
    for r in rows:
        if r["utc"] != at_utc or r.get("reaction", "baseline") != "baseline":
            continue
        key = (r["from"], r["to"])
        if key in found:
            raise SystemExit(f"error: two baseline rows for {key[0]} -> {key[1]} at {at_utc}")
        found[key] = BaselineRoute(
            state=r.get("state") or ("delivered" if r["status"] == "ok" else "no_route"),
            drop_cause=r["drop_cause"],
            satellites=tuple(int(x) for x in r["route"].split()),
            gateway=r["gateway"],
            gateway_source=r["gateway_source"],
            one_way_ms=float(r["one_way_ms"]) if r["one_way_ms"] else None,
        )
    return found


def read_routes_csv(path: Path) -> list[dict]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


# ---------------------------------------------------------------------------------------
# Hand-picked scenarios.
# ---------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Scenario:
    """What a hand-picked scenario fails, or why it was skipped."""

    failures: tuple[Failure, ...]
    skipped: str = ""  # the reason, when there is no failure
    satellites: int = 0
    links: int = 0
    gateways: int = 0
    element: str = ""  # the one element of a single-element scenario, as the program names it


def skipped(reason: str) -> Scenario:
    return Scenario((), skipped=reason)


def hand_picked(
    scenario: str,
    route: BaselineRoute | None,
    model: str,
    at_utc: str,
    planes: dict[int, tuple[str, int, int]],
    points=None,
    length_s: float | None = None,
) -> Scenario:
    """The failures of one scenario for one direction at at_utc. `points` is a function of no
    argument returning the sub-satellite points of every satellite in planes at at_utc (only
    called for `region`)."""
    if scenario not in SCENARIOS:
        raise ValueError(f"unknown scenario {scenario!r}")
    if scenario == "gateway" and model != "realistic":
        raise ValueError("the gateway scenario is for the realistic model only")
    if route is None:
        return skipped("no baseline row at the failure instant")
    if route.state != "delivered":
        return skipped(f"no baseline route at the failure instant ({route.state}, "
                       f"{route.drop_cause or 'no cause'})")  # fmt: skip
    sats = route.satellites
    if not sats:
        return skipped("the baseline route has no satellite")
    middle = sats[len(sats) // 2]
    for s in sats:
        if s not in planes:
            raise SystemExit(f"error: route satellite {s} is not in a plane")
    if scenario == "route-satellite":
        return Scenario(
            tuple(satellite_failures([middle], at_utc, length_s)),
            satellites=1,
            element=f"satellite:{middle}",
        )
    if scenario == "access-satellite":
        return Scenario(
            tuple(satellite_failures([sats[0]], at_utc, length_s)),
            satellites=1,
            element=f"satellite:{sats[0]}",
        )
    if scenario == "route-laser":
        if len(sats) < 2:
            return skipped("the baseline route has no laser hop (one satellite)")
        a, b = sorted((sats[len(sats) // 2 - 1], middle))
        return Scenario(
            (Failure("laser_link", at_utc, a=a, b=b, length_s=length_s),),
            links=1,
            element=f"link:{a}-{b}",
        )
    if scenario == "plane":
        shell, plane, _ = planes[middle]
        members = [s for s, p in planes.items() if p[0] == shell and p[1] == plane]
        return Scenario(
            tuple(satellite_failures(members, at_utc, length_s)), satellites=len(members)
        )
    if scenario == "region":
        where = points()
        here = where[middle]
        members = [s for s in planes if great_circle_km(*here, *where[s]) <= REGION_RADIUS_KM]
        return Scenario(
            tuple(satellite_failures(members, at_utc, length_s)), satellites=len(members)
        )
    # gateway
    if not route.gateway:
        return skipped("the baseline route uses no gateway")
    return Scenario(
        (Failure("gateway", at_utc, gateway=route.gateway, length_s=length_s),),
        gateways=1,
        element=f"gateway:{route.gateway}",
    )


# ---------------------------------------------------------------------------------------
# Random shares and the attack.
# ---------------------------------------------------------------------------------------
def share_count(share: float, n: int) -> int:
    return round(share * n)


def random_satellites(planes: dict[int, tuple], share: float, seed: int) -> list[int]:
    ids = np.array(sorted(planes), dtype=np.int64)
    chosen = np.random.default_rng(seed).choice(ids, share_count(share, len(ids)), replace=False)
    return sorted(int(x) for x in chosen)


def random_links(links: list[tuple[int, int]], share: float, seed: int) -> list[tuple[int, int]]:
    ordered = np.array(sorted(links), dtype=np.int64).reshape(-1, 2)
    chosen = np.random.default_rng(seed).choice(
        ordered, share_count(share, len(ordered)), replace=False, axis=0
    )
    return sorted((int(a), int(b)) for a, b in chosen)


def route_load(routes: Iterable[BaselineRoute]) -> Counter:
    """Satellite -> the number of delivered routes that pass through it."""
    load = Counter()
    for r in routes:
        if r.state == "delivered":
            load.update(set(r.satellites))
    return load


def attack_satellites(planes: dict[int, tuple], load: Counter, share: float) -> list[int]:
    """The share of satellites in planes that carry the most routes; ties to the lower number."""
    ranked = sorted(planes, key=lambda s: (-load.get(s, 0), s))
    return sorted(ranked[: share_count(share, len(ranked))])


def link_failures(links: Iterable[tuple[int, int]], start_utc: str) -> list[Failure]:
    return [Failure("laser_link", start_utc, a=a, b=b) for a, b in sorted(links)]


# ---------------------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Write Phase 4 failure files.")
    sub = parser.add_subparsers(dest="what", required=True)
    rs = sub.add_parser("random-satellites")
    rs.add_argument("--constellation-csv", type=Path, required=True)
    rl = sub.add_parser("random-lasers")
    rl.add_argument("--links-file", type=Path, required=True)
    for p in (rs, rl):
        p.add_argument("--share", type=float, required=True)
        p.add_argument("--seed", type=int, required=True)
        p.add_argument("--step-utc", required=True, help="the run's one step")
    hp = sub.add_parser("hand-picked")
    hp.add_argument("--scenario", choices=SCENARIOS, required=True)
    hp.add_argument("--from", dest="src", required=True)
    hp.add_argument("--to", dest="dst", required=True)
    hp.add_argument("--model", choices=("realistic", "best-case"), required=True)
    hp.add_argument("--propagate", type=Path, help="needed for region")
    hp.add_argument("--snapshot", type=Path, default=st.DEFAULT_SNAPSHOT)
    hp.add_argument("--length-s", type=float, help="transient: the failure's length")
    at = sub.add_parser("attack")
    at.add_argument("--share", type=float, required=True)
    for p in (hp, at):
        p.add_argument("--constellation-csv", type=Path, required=True)
        p.add_argument("--routes-csv", type=Path, required=True)
        p.add_argument("--at", required=True, help="the instant of the baseline routes")
    for p in (rs, rl, hp, at):
        p.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)

    if args.what == "random-satellites":
        planes = read_planes(args.constellation_csv)
        chosen = random_satellites(planes, args.share, args.seed)
        start = shifted_utc(args.step_utc, -OLD_NEWS_S)
        write_failure_file(
            args.out,
            satellite_failures(chosen, start),
            [
                f"random satellites: share {args.share} of {len(planes)} in planes, "
                f"{len(chosen)} drawn, seed {args.seed}",
                f"step {args.step_utc}; failures dated {OLD_NEWS_S} s before it",
            ],
        )
    elif args.what == "random-lasers":
        links = read_links(args.links_file)
        chosen = random_links(links, args.share, args.seed)
        start = shifted_utc(args.step_utc, -OLD_NEWS_S)
        write_failure_file(
            args.out,
            link_failures(chosen, start),
            [
                f"random laser links: share {args.share} of {len(links)} planned, "
                f"{len(chosen)} drawn, seed {args.seed}",
                f"step {args.step_utc}; failures dated {OLD_NEWS_S} s before it",
            ],
        )
    elif args.what == "hand-picked":
        planes = read_planes(args.constellation_csv)
        routes = baseline_routes(read_routes_csv(args.routes_csv), args.at)
        if args.scenario == "region" and args.propagate is None:
            parser.error("region needs --propagate")

        def points():
            return subsatellite_points(
                args.propagate, args.snapshot, args.at, sorted(planes), args.out.with_suffix(".p")
            )

        s = hand_picked(
            args.scenario,
            routes.get((args.src, args.dst)),
            args.model,
            args.at,
            planes,
            points,
            args.length_s,
        )
        if s.skipped:
            print(f"skipped: {s.skipped}", file=sys.stderr)
            return 1
        write_failure_file(
            args.out,
            list(s.failures),
            [
                f"hand-picked scenario {args.scenario}, {args.src} -> {args.dst}, "
                f"{args.model}, instant {args.at}",
                "permanent" if args.length_s is None else f"transient, {args.length_s} s",
            ],
        )
    else:
        planes = read_planes(args.constellation_csv)
        routes = baseline_routes(read_routes_csv(args.routes_csv), args.at)
        chosen = attack_satellites(planes, route_load(routes.values()), args.share)
        start = shifted_utc(args.at, -OLD_NEWS_S)
        write_failure_file(
            args.out,
            satellite_failures(chosen, start),
            [
                f"attack: share {args.share} of {len(planes)} satellites in planes, "
                f"{len(chosen)} most loaded by {len(routes)} baseline routes at {args.at}",
                f"failures dated {OLD_NEWS_S} s before it",
            ],
        )
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Run the two comparisons of docs/phase-4-failures.md 6.4 that are reported with no pass or fail.

Run by hand, not in CI. Both run the `failures` program on Hypatia's ideal shell (72 planes of
22 satellites, python/make_ideal_shell.py, written with both four-column link files:
links-plus-grid-sides.csv for +Grid and links-three.csv for `three`), best case, from the
shell's epoch (2000-01-01T00:00:00, also the plan time), 1 s steps, Hypatia's ground range
(--max-ground-range-km 1089.686), lowest-delay, one way. Per-run rows go to an output directory
that is not committed (default results/phase-4/comparisons/), the summary tables to
docs/phase-4-data/. Every run must exit 0 with "checks: PASSED"; otherwise the script stops,
printing the command and the program's output (exit status 1).

  starglider   StarGlider's 7% (Vissicchio and Handley, arXiv 2401.11490): one link failed on
               the shortest path, and the share of runs whose best backup path from the
               satellite next to the failed link costs over 20% more delay. Per link file:
                 1. one baseline run (no failure file) of all 4,950 pairs of Hypatia's 100
                    cities (data/ground/hypatia/cities.json) one way, for the baseline route of
                    every pair at the failure instant;
                 2. --draws draws (default 1,000) with numpy.random.default_rng(0): a pair
                    uniformly among the 4,950 (rng.integers), then one laser link of its
                    baseline route uniformly (rng.integers over its laser hops). A pair whose
                    baseline is not delivered at the failure instant, or whose route has no
                    laser hop (one satellite), is redrawn and counted;
                 3. per draw one run that carries only that pair (a cities file with its two
                    cities and the pair) and a failure file failing that link permanently at
                    5 s; --reactions local-repair, --planner start, re-plan every 15 s, 20 s
                    long, so the planner's next plan (15 s) comes after the repair and the
                    measurement. Draws that repeat a (pair, link) reuse its run (same inputs,
                    same output).
               Read per draw: the delay before the failure is the baseline lane's delay at the
               failure instant (routes CSV, 5 s: the route the local-repair lane carries up to
               then, checked equal to it in the events CSV); the repaired delay is the delay of
               the local-repair lane's first delivered interval that starts at or after the
               failure and before the next plan (events CSV; an interval's delay is the one at
               its start, the repair instant, 5.1 s with the default detection time). No such
               interval: no backup (a black hole until the planner acts). Ratio = repaired /
               before. Writes starglider-<links>.csv per draw and docs/phase-4-data/
               starglider.csv (per link file: draws, redrawn pairs, share over 20% of all draws
               and of the repaired ones, the published 0.07, share with no backup, the ratio's
               min, median, p90, p95, p99 and max over the repaired draws).
  percolation  The largest piece against the failed share, next to the thresholds of endless
               grids: square grid, bond 0.5 and site 0.5927 kept; honeycomb (what `three` makes
               of an ideal shell), bond 0.6527 and site 0.6970 kept. Written as failed shares:
               links 0.5 and satellites 0.4073 for +Grid, 0.3473 and 0.3030 for `three`. Per
               link file, kind (satellites, laser links), failed share 0.05 to 0.80 in steps of
               0.05 and seed 0 to 19: the share drawn as python/make_failures.py draws it
               (numpy.random.default_rng(seed).choice without replacement on the sorted list,
               round(share x n) elements; satellites by catalogue number, links as (lower,
               higher)), every failure permanent and dated one hour before the run's one step
               (old news), --duration-s 0, --reactions planner, --repoint off, Tokyo -> Delhi
               as the one pair, --network-out; read the `all` row: largest_piece_share (of the
               working satellites). Writes percolation-runs.csv per run and
               docs/phase-4-data/percolation.csv (per link file, kind and share: seeds, mean,
               min and max of the largest piece share, the theoretical threshold), and prints
               the first failed share at which the mean drops below one half.

Differences from StarGlider's setup, all named (a comparison, not a reproduction):
  - Random pairs of Hypatia's cities, not random pairs of satellites: every route starts and
    ends with a radio hop to a city, the delay includes both ground links, and pairs are
    weighted by where the cities are (most in the northern mid-latitudes), not uniformly over
    the shell. A route with one satellite has no laser link to fail; such pairs are redrawn.
  - Hypatia's shell (72 x 22 at 53 degrees, 15.19 revolutions a day, about 510 km, half-slot
    staggered planes) with +Grid, and `three` as a second variant; StarGlider's own shell is
    ambiguous in its text, so it is not matched.
  - The delay is propagation only (no queueing, no per-hop processing: --hop-delay-ms is the
    program's default), at the speed of light along straight segments.
  - The failed link is drawn uniformly among the route's laser links; the two ground links are
    never failed.
  - The backup is this program's local repair (spec 4.5): the shortest path in the planning
    graph from the near end of the dead link to the route's end avoiding the next node of the
    route and every node before the repairing one. It therefore also avoids the far end of the
    dead link as a node, not only the link, and it must end at the same destination city over
    a radio hop (the destination's ground links are in the planning graph). So when the dead
    link is the route's last laser hop and the destination city sees no other satellite in
    range, there is no backup, where a link-only backup might still exist.
  - The planning graph is that of the plan at 0 s (`--planner start`, one step's geometry);
    the repaired delay is taken at the repair instant (the failure plus the detection time,
    0.1 s), the delay before at the failure instant. Both are true delays of the geometry at
    those instants, while both routes were chosen on the 0 s graph, so the route held at 5 s
    is not always the lowest-delay one any more and a ratio can fall slightly below 1.
  - The baseline route is the lowest-delay route of the program (lowest total delay, Phase 3),
    not a hop-count shortest path.
  - The route at 5 s is the one planned at 0 s and held (Phase 3); a pair whose held route lost
    a ground link by 5 s (Hypatia's 1,089.686 km range is tight) has no delivered route then
    and is redrawn and counted, as is a pair served by one satellite.
  - One instant (the shell's epoch plus 5 s) for every draw, not draws over time; the geometry
    of an ideal shell repeats, but the cities' positions under it do not.

The percolation runs differ from the endless grids by the shell's size (1,584 satellites,
3,168 +Grid or 2,376 `three` links), its wrap-around (plane 71 joins plane 0 again), and in
that the network CSV counts pieces on the true network's laser links that carry traffic at the
step: a planned link out of line of sight would count as missing (on the ideal shell at its
epoch none is). Checked on every run: the program's working satellites, pieces and largest
piece equal those of the link file without the failed elements (union-find here), and with
nothing failed the shell is one piece of 1,584.

Percentiles are numpy's default (linear interpolation). Exit status 1 if a run fails or its
checks do not pass, 2 on a usage error.

Usage (inside the CI image, with the failures program built as scripts/ci.sh
validate-failures does):

    python3 python/compare_failures.py --failures-program build/validate/bin/failures \\
        starglider [--draws 1000] [--links plus-grid,three] [--jobs 2]
    python3 python/compare_failures.py --failures-program build/validate/bin/failures \\
        percolation [--seeds 20] [--links plus-grid,three] [--jobs 2]
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from functools import partial
from pathlib import Path

import numpy as np

import make_failures as mf
import make_ideal_shell as ideal
import snapshot_tools as st
from validate_routing import HYPATIA_RANGE_KM

EPOCH_UTC = ideal.EPOCH[:19]
DOCS_DATA = st.REPO_ROOT / "docs/phase-4-data"
LINK_FILES = {"plus-grid": ideal.PLUS_GRID_SIDES_FILE, "three": ideal.THREE_LINKS_FILE}

# StarGlider.
PUBLISHED_SHARE = 0.07
OVER = 1.20  # "over 20% more delay"
FAILURE_S = 5
DURATION_S = 20
REPLAN_S = 15
STARGLIDER_SEED = 0
QUANTILES = (("min", 0), ("median", 50), ("p90", 90), ("p95", 95), ("p99", 99), ("max", 100))

# Percolation: thresholds of endless grids as the share kept (bond, site), and as failed.
KEPT_THRESHOLD = {
    ("plus-grid", "laser-links"): 0.5,
    ("plus-grid", "satellites"): 0.5927,
    ("three", "laser-links"): 0.6527,
    ("three", "satellites"): 0.6970,
}
SHARES = [round(0.05 * k, 2) for k in range(1, 17)]
KINDS = ("satellites", "laser-links")
PERCOLATION_PAIR = ("Tokyo", "Delhi")


class RunError(Exception):
    pass


# ---------------------------------------------------------------------------------------
# Running the program.
# ---------------------------------------------------------------------------------------
def failures_command(args, links: str, cities: Path, duration_s: int, extra: list[str]):
    shell = args.out / "hypatia-shell"
    return [
        str(args.failures_program),
        "--snapshot", str(shell),
        "--ground", str(args.ground),
        "--cities", str(cities),
        "--pattern", links,
        "--model", "best-case",
        "--shells-file", str(shell / ideal.SHELLS_FILE),
        "--links-file", str(shell / LINK_FILES[links]),
        "--start", EPOCH_UTC,
        "--plan-time", EPOCH_UTC,
        "--duration-s", str(duration_s),
        "--step-s", "1",
        "--strategies", "lowest-delay",
        "--directions", "forward",
        "--roles", "all",
        "--max-ground-range-km", repr(HYPATIA_RANGE_KM),
        *extra,
    ]  # fmt: skip


def run(command: list[str]) -> float:
    """Runs one `failures` command; returns its wall time. Raises RunError unless it exits 0
    with its checks passed (the summary goes to standard output, the routes CSV to a file)."""
    began = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    seconds = time.monotonic() - began
    if result.returncode != 0 or "checks: PASSED" not in result.stdout:
        raise RunError(
            f"failures exited with {result.returncode}\ncommand: {' '.join(command)}\n"
            f"{result.stdout}{result.stderr}"
        )
    return seconds


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def write_cities(path: Path, records: list[dict], pairs: list[tuple[str, str]]) -> None:
    keep = [{k: c[k] for k in ("name", "country", "region", "geofeed_city", "lat", "lon")}
            for c in records]  # fmt: skip
    body = {"cities": keep, "pairs": [{"from": a, "to": b, "role": "study"} for a, b in pairs]}
    path.write_text(json.dumps(body, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")


def prepare_shell(args) -> None:
    shell = args.out / "hypatia-shell"
    if not all((shell / f).is_file() for f in LINK_FILES.values()):
        ideal.write_ideal_shell(shell, st.DEFAULT_SNAPSHOT / st.EOP_FILE, ideal.HYPATIA)
        ideal.write_four_column_links(shell, ideal.HYPATIA)


def at_utc(seconds: int) -> str:
    return mf.shifted_utc(EPOCH_UTC, seconds)


def same_utc(a: str, b: str) -> bool:
    return st.parse_utc(a) == st.parse_utc(b)


def route_of(text: str) -> tuple[int, ...]:
    return tuple(int(x) for x in text.split()) if text else ()


def quantiles(values: list[float]) -> dict[str, float | None]:
    if not values:
        return {name: None for name, _ in QUANTILES}
    v = np.array(values)
    return {name: float(np.percentile(v, q)) for name, q in QUANTILES}


def number(x) -> str:
    return "" if x is None else f"{x:.6g}" if isinstance(x, float) else str(x)


def in_parallel(jobs: int, work, items: list):
    """work(item) for every item, at most `jobs` at a time, results in the items' order."""
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        return list(pool.map(work, items))


# ---------------------------------------------------------------------------------------
# StarGlider's 7%.
# ---------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Draw:
    number: int
    source: str
    destination: str
    route: tuple[int, ...]  # Satellites, from the sending end.
    hop: int  # The failed link joins route[hop] (the near end) and route[hop + 1].


@dataclass(frozen=True)
class Outcome:
    before_ms: float
    repair_s: float | None
    repaired_ms: float | None
    repaired_route: tuple[int, ...]
    state_after: str  # The local-repair lane's state just after the failure.

    @property
    def ratio(self) -> float | None:
        return None if self.repaired_ms is None else self.repaired_ms / self.before_ms


def baseline_routes(args, links: str, records, pairs, work: Path) -> dict:
    """(from, to) -> (state, route, one_way_ms) of the baseline at the failure instant."""
    cities = work / "cities-all.json"
    write_cities(cities, records, pairs)
    routes = work / f"baseline-{links}-routes.csv"
    command = failures_command(
        args,
        links,
        cities,
        DURATION_S,
        ["--reactions", "local-repair", "--planner", "start", "--replan-s", str(REPLAN_S),
         "--out", str(routes)],
    )  # fmt: skip
    seconds = run(command)
    print(f"  baseline run of {len(pairs)} pairs: {seconds:.1f} s")
    found = {}
    states: dict[str, int] = {}
    when = at_utc(FAILURE_S)
    for r in read_csv(routes):
        if r["reaction"] == "baseline" and same_utc(r["utc"], when):
            found[(r["from"], r["to"])] = (
                r["state"],
                route_of(r["route"]),
                float(r["one_way_ms"]) if r["one_way_ms"] else None,
            )
            key = r["state"] + (f" ({r['drop_cause']})" if r["drop_cause"] else "")
            states[key] = states.get(key, 0) + 1
    routes.unlink()
    if set(found) != set(pairs):
        raise RunError(f"the baseline run has rows for {len(found)} of {len(pairs)} pairs")
    single = sum(1 for s, route, _ in found.values() if s == "delivered" and len(route) < 2)
    print(
        f"  baseline at {FAILURE_S} s, pairs by state: {dict(sorted(states.items()))}; "
        f"delivered over one satellite (no laser hop): {single}"
    )
    return found


def draw_pairs(routes: dict, pairs: list, draws: int) -> tuple[list[Draw], int, int]:
    """The draws, the pairs redrawn for no delivered route and those for no laser hop."""
    rng = np.random.default_rng(STARGLIDER_SEED)
    out: list[Draw] = []
    undelivered = single = 0
    while len(out) < draws:
        a, b = pairs[int(rng.integers(len(pairs)))]
        state, route, _ = routes[(a, b)]
        if state != "delivered":
            undelivered += 1
            continue
        if len(route) < 2:
            single += 1
            continue
        out.append(Draw(len(out), a, b, route, int(rng.integers(len(route) - 1))))
    return out, undelivered, single


def one_draw(args, links: str, records_by_name: dict, d: Draw, *, work: Path) -> tuple:
    with tempfile.TemporaryDirectory(dir=work) as tmp:
        tmp_dir = Path(tmp)
        cities = tmp_dir / "cities.json"
        write_cities(
            cities, [records_by_name[d.source], records_by_name[d.destination]],
            [(d.source, d.destination)],
        )  # fmt: skip
        failures = tmp_dir / "failures.csv"
        a, b = d.route[d.hop], d.route[d.hop + 1]
        mf.write_failure_file(
            failures,
            [mf.Failure("laser_link", at_utc(FAILURE_S), a=a, b=b)],
            [f"StarGlider comparison, draw {d.number}: {d.source} -> {d.destination}, "
             f"laser hop {d.hop} of its baseline route at {FAILURE_S} s, permanent"],
        )  # fmt: skip
        routes, events = tmp_dir / "routes.csv", tmp_dir / "events.csv"
        command = failures_command(
            args,
            links,
            cities,
            DURATION_S,
            ["--reactions", "local-repair", "--planner", "start", "--replan-s", str(REPLAN_S),
             "--failures", str(failures), "--out", str(routes), "--events-out", str(events)],
        )  # fmt: skip
        seconds = run(command)
        return outcome_of(read_csv(routes), read_csv(events), d), seconds


def outcome_of(routes: list[dict], events: list[dict], d: Draw) -> Outcome:
    when = at_utc(FAILURE_S)
    base = [r for r in routes if r["reaction"] == "baseline" and same_utc(r["utc"], when)]
    if len(base) != 1 or base[0]["state"] != "delivered" or route_of(base[0]["route"]) != d.route:
        raise RunError(f"draw {d.number}: the baseline at {FAILURE_S} s is not the drawn route")
    lane = sorted(
        (r for r in events if r["reaction"] == "local-repair"), key=lambda r: float(r["start_s"])
    )
    before = [r for r in lane if float(r["start_s"]) < FAILURE_S <= float(r["end_s"])]
    if (
        len(before) != 1
        or before[0]["state"] != "delivered"
        or route_of(before[0]["route"]) != d.route
    ):
        raise RunError(f"draw {d.number}: the local-repair lane before the failure is not the "
                       "baseline route")  # fmt: skip
    after = [r for r in lane if FAILURE_S <= float(r["start_s"]) < REPLAN_S]
    if not after:
        raise RunError(f"draw {d.number}: the local-repair lane has no interval after the failure")
    repaired = next((r for r in after if r["state"] == "delivered"), None)
    return Outcome(
        before_ms=float(base[0]["one_way_ms"]),
        repair_s=None if repaired is None else float(repaired["start_s"]),
        repaired_ms=None if repaired is None else float(repaired["one_way_ms"]),
        repaired_route=() if repaired is None else route_of(repaired["route"]),
        state_after=after[0]["state"],
    )


def run_starglider(args) -> int:
    data = json.loads(args.hypatia_cities.read_text(encoding="utf-8"))
    records = data["cities"]
    by_name = {c["name"]: c for c in records}
    pairs = [(p["from"], p["to"]) for p in data["pairs"]]
    work = args.out / "work"
    work.mkdir(parents=True, exist_ok=True)
    rows = []
    for links in args.links:
        print(f"starglider, {links}:")
        began = time.monotonic()
        routes = baseline_routes(args, links, records, pairs, work)
        draws, undelivered, single = draw_pairs(routes, pairs, args.draws)
        first: dict[tuple, Draw] = {}  # (from, to, hop) -> its first draw, in draw order.
        for d in draws:
            first.setdefault((d.source, d.destination, d.hop), d)
        keys = list(first)
        runs = in_parallel(
            args.jobs, partial(one_draw, args, links, by_name, work=work), list(first.values())
        )
        results = dict(zip(keys, runs, strict=True))
        total = time.monotonic() - began
        times = [s for _, s in results.values()]
        outcomes = [results[(d.source, d.destination, d.hop)][0] for d in draws]
        with (args.out / f"starglider-{links}.csv").open("w", newline="", encoding="utf-8") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(["draw", "from", "to", "route_satellites", "failed_hop", "failed_link",
                        "before_ms", "state_after_failure", "repair_s", "repaired_ms", "ratio",
                        "over_20pct", "no_backup", "route", "repaired_route"])  # fmt: skip
            for d, o in zip(draws, outcomes, strict=True):
                a, b = sorted((d.route[d.hop], d.route[d.hop + 1]))
                w.writerow(
                    [d.number, d.source, d.destination, len(d.route), d.hop, f"{a}-{b}",
                     repr(o.before_ms), o.state_after, number(o.repair_s),
                     "" if o.repaired_ms is None else repr(o.repaired_ms),
                     "" if o.ratio is None else repr(o.ratio),
                     "" if o.ratio is None else int(o.ratio > OVER), int(o.ratio is None),
                     " ".join(map(str, d.route)), " ".join(map(str, o.repaired_route))]
                )  # fmt: skip
        ratios = [o.ratio for o in outcomes if o.ratio is not None]
        over = sum(1 for x in ratios if x > OVER)
        no_backup = len(outcomes) - len(ratios)
        q = quantiles(ratios)
        row = {
            "links": links,
            "draws": len(draws),
            "redrawn_no_route": undelivered,
            "redrawn_no_laser_hop": single,
            "distinct_runs": len(keys),
            "share_over_20pct": over / len(draws),
            "share_over_20pct_of_repaired": over / len(ratios) if ratios else None,
            "published_share": PUBLISHED_SHARE,
            "share_no_backup": no_backup / len(draws),
            **{f"ratio_{k}": v for k, v in q.items()},
        }
        rows.append(row)
        print(
            f"  {len(draws)} draws ({len(keys)} distinct runs), redrawn {undelivered} without a "
            f"delivered route and {single} without a laser hop; over 20%: {over} "
            f"({row['share_over_20pct']:.1%}; published 7%), no backup: {no_backup} "
            f"({row['share_no_backup']:.1%})"
        )
        print("  ratio: " + ", ".join(f"{k} {number(v)}" for k, v in q.items()))
        instants = sorted({o.repair_s for o in outcomes if o.repair_s is not None})
        states = sorted({o.state_after for o in outcomes})
        print(f"  repair instants (s): {instants}; lane state just after the failure: {states}")
        print(
            f"  time: {total:.0f} s in all, one draw's run {np.median(times):.2f} s median "
            f"({min(times):.2f} to {max(times):.2f}), {args.jobs} at a time"
        )
    write_table(DOCS_DATA / "starglider.csv", rows)
    return 0


def write_table(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(list(rows[0]))
        for r in rows:
            w.writerow([number(v) for v in r.values()])


# ---------------------------------------------------------------------------------------
# Percolation.
# ---------------------------------------------------------------------------------------
def percolation_run(args, links: str, kind: str, share: float, seed: int, cities: Path,
                    work: Path) -> tuple[dict, float]:  # fmt: skip
    shell = args.out / "hypatia-shell"
    when = mf.shifted_utc(EPOCH_UTC, -mf.OLD_NEWS_S)
    ids = [x.norad_cat_id for x in ideal.all_slots(ideal.HYPATIA)]
    planned = mf.read_links(shell / LINK_FILES[links])
    if kind == "satellites":
        dead = mf.random_satellites(dict.fromkeys(ids), share, seed)
        failed = mf.satellite_failures(dead, when)
        expected = pieces_of(sorted(set(ids) - set(dead)), planned)
    else:
        dead_links = mf.random_links(planned, share, seed)
        failed = mf.link_failures(dead_links, when)
        expected = pieces_of(ids, sorted(set(planned) - set(dead_links)))
    with tempfile.TemporaryDirectory(dir=work) as tmp:
        tmp_dir = Path(tmp)
        failures = tmp_dir / "failures.csv"
        mf.write_failure_file(
            failures,
            failed,
            [f"percolation: {links}, {share:g} of the {kind} failed, seed {seed}, old news"],
        )
        network, routes = tmp_dir / "network.csv", tmp_dir / "routes.csv"
        extra = ["--reactions", "planner", "--replan-s", "1", "--out", str(routes)]
        if failed:
            extra += ["--failures", str(failures)]
        extra += ["--network-out", str(network)]
        seconds = run(failures_command(args, links, cities, 0, extra))
        rows = [r for r in read_csv(network) if r["scope"] == "all"]
    if len(rows) != 1:
        raise RunError(f"percolation {links} {kind} {share} {seed}: {len(rows)} `all` rows")
    r = rows[0]
    got = (int(r["working_satellites"]), int(r["pieces"]), int(r["largest_piece"]))
    if got != expected:
        raise RunError(
            f"percolation {links} {kind} {share} {seed}: the network CSV says (working, pieces, "
            f"largest) {got}, the link file without the failed elements {expected}"
        )
    return {
        "links": links,
        "kind": kind,
        "share": share,
        "seed": seed,
        "failed": len(failed),
        "working_satellites": int(r["working_satellites"]),
        "pieces": int(r["pieces"]),
        "largest_piece": int(r["largest_piece"]),
        "largest_piece_share": float(r["largest_piece_share"]),
    }, seconds


def pieces_of(nodes: list[int], links: list[tuple[int, int]]) -> tuple[int, int, int]:
    """(nodes, pieces, largest piece) of the graph of `links` between `nodes` (union-find);
    links with an end outside `nodes` are left out. The cross-check of every run: on the ideal
    shell at its epoch every planned link is in line of sight, so the true network's links
    that carry traffic are the planned ones without the failed elements."""
    parent = {x: x for x in nodes}

    def root(x: int) -> int:
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b in links:
        if a in parent and b in parent:
            ra, rb = root(a), root(b)
            if ra != rb:
                parent[max(ra, rb)] = min(ra, rb)
    sizes: dict[int, int] = {}
    for x in nodes:
        sizes[root(x)] = sizes.get(root(x), 0) + 1
    return len(nodes), len(sizes), max(sizes.values(), default=0)


def run_percolation(args) -> int:
    data = json.loads(args.hypatia_cities.read_text(encoding="utf-8"))
    by_name = {c["name"]: c for c in data["cities"]}
    work = args.out / "work"
    work.mkdir(parents=True, exist_ok=True)
    cities = work / "cities-percolation.json"
    write_cities(cities, [by_name[x] for x in PERCOLATION_PAIR], [PERCOLATION_PAIR])

    # Nothing failed: the whole shell is one piece (the counts are what is expected).
    for links in args.links:
        r, seconds = percolation_run(args, links, "satellites", 0.0, 0, cities, work)
        print(f"{links}, nothing failed: {r['working_satellites']} working, {r['pieces']} "
              f"piece(s), largest {r['largest_piece']} ({seconds:.2f} s)")  # fmt: skip
        if r["pieces"] != 1 or r["largest_piece"] != len(ideal.all_slots(ideal.HYPATIA)):
            raise RunError(f"{links}: with nothing failed the shell is not one whole piece")

    jobs = [(links, kind, share, seed) for links in args.links for kind in KINDS
            for share in SHARES for seed in range(args.seeds)]  # fmt: skip
    began = time.monotonic()
    done = in_parallel(args.jobs, lambda j: percolation_run(args, *j, cities, work), jobs)
    total = time.monotonic() - began
    times = [s for _, s in done]
    runs = [r for r, _ in done]
    with (args.out / "percolation-runs.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(list(runs[0]))
        for r in runs:
            w.writerow([repr(v) if isinstance(v, float) else v for v in r.values()])

    rows = []
    for links in args.links:
        for kind in KINDS:
            threshold = round(1.0 - KEPT_THRESHOLD[(links, kind)], 4)
            below = None
            for share in SHARES:
                v = [r["largest_piece_share"] for r in runs
                     if (r["links"], r["kind"], r["share"]) == (links, kind, share)]  # fmt: skip
                mean = float(np.mean(v))
                if below is None and mean < 0.5:
                    below = share
                rows.append({"links": links, "kind": kind, "failed_share": share,
                             "seeds": len(v), "mean_largest_piece_share": mean,
                             "min_largest_piece_share": min(v),
                             "max_largest_piece_share": max(v),
                             "threshold_failed_share": threshold})  # fmt: skip
            line = " ".join(f"{r['mean_largest_piece_share']:.3f}" for r in rows[-len(SHARES) :])
            print(f"{links}, {kind}: mean largest piece share at 0.05..0.80: {line}")
            print(f"  first failed share with the mean below one half: "
                  f"{below if below is not None else 'none up to 0.80'} (threshold of the "
                  f"endless grid {threshold:g})")  # fmt: skip
    write_table(DOCS_DATA / "percolation.csv", rows)
    print(f"time: {total:.0f} s for {len(jobs)} runs, one run {np.median(times):.2f} s median "
          f"({min(times):.2f} to {max(times):.2f}), {args.jobs} at a time")  # fmt: skip
    return 0


def main(argv: list[str]) -> int:
    st.forbid_network()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--failures-program", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=st.REPO_ROOT / "results/phase-4/comparisons")
    parser.add_argument("--ground", type=Path, default=st.REPO_ROOT / "data/ground/2026-11-19")
    parser.add_argument(
        "--hypatia-cities", type=Path, default=st.REPO_ROOT / "data/ground/hypatia/cities.json"
    )
    parser.add_argument("--links", default="plus-grid,three", help="link files: plus-grid,three")
    parser.add_argument("--jobs", type=int, default=2, help="runs at a time (default 2)")
    parser.add_argument("--draws", type=int, default=1000, help="starglider: draws per link file")
    parser.add_argument("--seeds", type=int, default=20, help="percolation: seeds per share")
    parser.add_argument("comparison", choices=["starglider", "percolation"])
    args = parser.parse_args(argv)
    args.links = args.links.split(",")
    if not args.failures_program.is_file():
        print(f"error: {args.failures_program} not found", file=sys.stderr)
        return 2
    if set(args.links) - set(LINK_FILES) or args.jobs < 1 or args.draws < 1 or args.seeds < 1:
        print("error: --links takes plus-grid,three; --jobs, --draws, --seeds >= 1",
              file=sys.stderr)  # fmt: skip
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    prepare_shell(args)
    try:
        return run_starglider(args) if args.comparison == "starglider" else run_percolation(args)
    except RunError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

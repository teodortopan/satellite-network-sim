#!/usr/bin/env python3
"""Run the routing program on the two published setups of docs/phase-3-routing.md 6.3.

Run by hand, not in CI (the program-against-model comparison is python/validate_routing.py).
Writes into an output directory that is not committed (default results/phase-3/validation/)
and a short summary table under docs/phase-3-data/.

  hypatia     Kassing et al., IMC 2020, Figure 8a. Hypatia's shell and link file
              (python/make_ideal_shell.py), --max-ground-range-km 1089.686, Hypatia's 100
              cities (data/ground/hypatia/cities.json), all 4,950 pairs one way, 200 s at
              100 ms (2,000 instants, 0 to 199.9 s, as Hypatia's range(0, 200 s, 100 ms)),
              lowest-delay re-planned every step, best case. The routes CSV (about 2 GB) is
              streamed, not kept. Path changes per pair by Hypatia's rule (analyze_path.py):
              consecutive steps whose node lists differ, no route being the empty list.
              Writes hypatia_pairs.csv (per pair: changes by Hypatia's rule and by spec 4.7,
              drops, laser hops min and max) and docs/phase-3-data/hypatia_path_changes.csv.
  bariloche   Stock et al., arXiv 2408.07460, section IV-B. The Bariloche shell and link file,
              --min-elevation-deg 40, --hop-delay-ms 1.012, Bariloche (-41.13, -71.31) to
              Beijing (39.90, 116.41), one orbit (5,739 s) at 1 s, re-planned every step,
              best case, the three strategies. The main run starts at the shell's epoch; the
              same run from five more starts spread over one orbit shows how much the counts
              depend on the start the paper does not give. Changes and bad changes by spec
              4.7 (both steps with a route, routes differ; bad when the delay goes down),
              also by Hypatia's rule. Writes bariloche_runs.csv and
              docs/phase-3-data/bariloche_beijing.csv.

Percentiles are numpy's default (linear interpolation). Exit status 1 if a run fails or its
checks do not pass, 2 on a usage error.

Usage (inside the CI image, after building routing as scripts/ci.sh validate-routing does):

    python3 python/reproduce_routing.py --routing build/validate/bin/routing hypatia
    python3 python/reproduce_routing.py --routing build/validate/bin/routing bariloche
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

import make_ideal_shell as ideal
import snapshot_tools as st
from validate_routing import (
    BARILOCHE,
    BEIJING,
    HYPATIA_RANGE_KM,
    Row,
    city_record,
    count_changes,
    hypatia_changes,
)

EPOCH = "2000-01-01T00:00:00"
DOCS_DATA = st.REPO_ROOT / "docs/phase-3-data"

HYPATIA_PUBLISHED = {"median": 4, "share_7_or_more": 0.10}
BARILOCHE_PUBLISHED = {
    "lowest-delay": (185, 101),
    "keep-until-broken": (103, 61),
    "longest-lived": (58, 29),
}
BARILOCHE_PUBLISHED_MEDIAN_MS = 89.0  # "just under 89 ms"
BARILOCHE_ORBIT_S = 5739
BARILOCHE_OFFSETS_S = [round(i * BARILOCHE_ORBIT_S / 6) for i in range(6)]
STRATEGIES = ("lowest-delay", "keep-until-broken", "longest-lived")


def routing_command(exe: Path, snapshot: Path, cities: Path, ground: Path, extra: list[str]):
    return [
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
        *extra,
    ]


# ---------------------------------------------------------------------------------------
def run_hypatia(args) -> int:
    snapshot = args.out / "hypatia-shell"
    ideal.write_ideal_shell(snapshot, st.DEFAULT_SNAPSHOT / st.EOP_FILE, ideal.HYPATIA)
    command = routing_command(
        args.routing,
        snapshot,
        args.hypatia_cities,
        args.ground,
        [
            "--start",
            EPOCH,
            "--duration-s",
            "199.9",
            "--step-s",
            "0.1",
            "--strategies",
            "lowest-delay",
            "--replan-s",
            "0.1",
            "--directions",
            "forward",
            "--max-ground-range-km",
            repr(HYPATIA_RANGE_KM),
            "--out",
            "-",
        ],
    )
    began = time.monotonic()
    summary_path = args.out / "hypatia.summary.txt"
    with summary_path.open("w", encoding="utf-8") as summary:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=summary, text=True)
        assert process.stdout is not None
        # Per pair, streamed: the last two rows (counted as they come), drops and hop counts.
        pairs: dict[tuple[str, str], dict] = {}
        for r in csv.DictReader(process.stdout):
            key = (r["from"], r["to"])
            route = tuple(int(x) for x in r["route"].split()) if r["route"] else ()
            row = Row(
                r["status"],
                r["drop_cause"],
                float(r["one_way_ms"]) if r["one_way_ms"] else None,
                None,
                route,
            )
            p = pairs.get(key)
            if p is None:
                p = pairs[key] = {"last": None, "steps": 0, "hyp": 0, "spec": 0, "drops": 0,
                                  "hops": set()}  # fmt: skip
            if p["last"] is not None:
                p["hyp"] += hypatia_changes([p["last"].route, route])
                p["spec"] += count_changes([p["last"], row]).changes
            p["last"] = row
            p["steps"] += 1
            if row.status == "ok":
                p["hops"].add(int(r["laser_hops"]) + 2)  # Hypatia counts the two GSLs.
            else:
                p["drops"] += 1
        status = process.wait()
    seconds = time.monotonic() - began
    text = summary_path.read_text(encoding="utf-8")
    if status != 0 or "checks: PASSED" not in text:
        print(f"error: routing exited with {status}; see {summary_path}", file=sys.stderr)
        return 1

    counts = []
    with (args.out / "hypatia_pairs.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(
            ["from", "to", "steps", "hypatia_changes", "spec_changes", "drop_steps",
             "min_hops", "max_hops"]
        )  # fmt: skip
        for (a, b), p in pairs.items():
            counts.append(p["hyp"])
            w.writerow(
                [a, b, p["steps"], p["hyp"], p["spec"], p["drops"],
                 min(p["hops"], default=""), max(p["hops"], default="")]
            )  # fmt: skip
    values = np.array(counts)
    steps = {p["steps"] for p in pairs.values()}
    result = {
        "pairs": len(counts),
        "steps": sorted(steps),
        "median": float(np.median(values)),
        "p90": float(np.percentile(values, 90)),
        "share_7_or_more": float(np.mean(values >= 7)),
        "mean": float(np.mean(values)),
        "max": int(values.max()),
        "pairs_with_drops": sum(1 for p in pairs.values() if p["drops"]),
    }
    print(f"hypatia: {result['pairs']} pairs, steps {result['steps']}, {seconds:.0f} s")
    print(
        f"  path changes per pair (Hypatia's rule): median {result['median']:g} "
        f"(published {HYPATIA_PUBLISHED['median']}), 90th percentile {result['p90']:g}, "
        f"share with 7 or more {result['share_7_or_more']:.1%} (published 10%), mean "
        f"{result['mean']:.2f}, max {result['max']}; pairs with a drop "
        f"{result['pairs_with_drops']}"
    )
    distribution = np.bincount(values)
    print(f"  pairs by number of changes: {dict(enumerate(distribution.tolist()))}")
    DOCS_DATA.mkdir(parents=True, exist_ok=True)
    with (DOCS_DATA / "hypatia_path_changes.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["measure", "ours", "published"])
        w.writerow(["pairs", result["pairs"], 4950])
        w.writerow(["median_path_changes", f"{result['median']:g}", 4])
        w.writerow(["p90_path_changes", f"{result['p90']:g}", ""])
        w.writerow(["share_pairs_7_or_more", f"{result['share_7_or_more']:.4f}", "0.10"])
        w.writerow(["mean_path_changes", f"{result['mean']:.3f}", ""])
        w.writerow(["max_path_changes", result["max"], ""])
        w.writerow(["pairs_with_a_drop", result["pairs_with_drops"], ""])
        for n, k in enumerate(distribution.tolist()):
            w.writerow([f"pairs_with_{n}_changes", k, ""])
    return 0


# ---------------------------------------------------------------------------------------
def bariloche_run(args, snapshot: Path, cities: Path, start: str) -> tuple[dict, str]:
    command = routing_command(
        args.routing,
        snapshot,
        cities,
        args.ground,
        [
            "--start",
            start,
            "--duration-s",
            str(BARILOCHE_ORBIT_S),
            "--step-s",
            "1",
            "--strategies",
            ",".join(STRATEGIES),
            "--replan-s",
            "1",
            "--directions",
            "forward",
            "--min-elevation-deg",
            "40",
            "--hop-delay-ms",
            "1.012",
            "--out",
            "-",
        ],
    )
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0 or "checks: PASSED" not in result.stderr:
        raise SystemExit(f"error: routing exited with {result.returncode}\n{result.stderr}")
    series: dict[str, list[Row]] = {s: [] for s in STRATEGIES}
    hops: dict[str, list[int]] = {s: [] for s in STRATEGIES}
    for r in csv.DictReader(io.StringIO(result.stdout)):
        route = tuple(int(x) for x in r["route"].split()) if r["route"] else ()
        one_way = float(r["one_way_ms"]) if r["one_way_ms"] else None
        series[r["strategy"]].append(Row(r["status"], r["drop_cause"], one_way, None, route))
        if r["status"] == "ok":
            hops[r["strategy"]].append(int(r["laser_hops"]))
    out = {}
    for s, rows in series.items():
        c = count_changes(rows)
        delays = [x.one_way_ms for x in rows if x.status == "ok"]
        causes: dict[str, int] = {}
        for x in rows:
            if x.status != "ok":
                causes[x.cause] = causes.get(x.cause, 0) + 1
        out[s] = {
            "steps": len(rows),
            "changes": c.changes,
            "bad": c.bad,
            "increases": c.increases,
            "breaks": c.breaks,
            "hypatia_changes": hypatia_changes([x.route for x in rows]),
            "drop_steps": sum(causes.values()),
            "drop_causes": causes,
            "median_ms": float(np.median(delays)) if delays else None,
            "min_ms": min(delays, default=None),
            "max_ms": max(delays, default=None),
            "median_laser_hops": float(np.median(hops[s])) if hops[s] else None,
        }
    return out, result.stderr


def run_bariloche(args) -> int:
    snapshot = args.out / "bariloche-shell"
    ideal.write_ideal_shell(snapshot, st.DEFAULT_SNAPSHOT / st.EOP_FILE, ideal.BARILOCHE)
    cities = args.out / "bariloche-cities.json"
    with cities.open("w", encoding="utf-8") as f:
        json.dump(
            {
                "cities": [city_record(*BARILOCHE), city_record(*BEIJING)],
                "pairs": [{"from": BARILOCHE[0], "to": BEIJING[0], "role": "study"}],
            },
            f,
        )
    epoch = st.parse_utc(EPOCH)
    runs = {}
    for offset in BARILOCHE_OFFSETS_S:
        start = epoch.plus_ns(offset * st.NS_PER_S).iso()
        runs[offset], summary = bariloche_run(args, snapshot, cities, start)
        (args.out / f"bariloche-{offset}.summary.txt").write_text(summary, encoding="utf-8")
        if offset == 0:
            m = re.search(r"Bariloche -> Beijing \(study, (\d+) km", summary)
            print(f"bariloche: great-circle distance {m.group(1) if m else '?'} km "
                  "(published 19,350 km)")  # fmt: skip
        line = ", ".join(
            f"{s} {r['changes']} ({r['bad']} bad), median {r['median_ms']:.2f} ms, drops "
            f"{r['drop_steps']}"
            for s, r in runs[offset].items()
        )
        print(f"  start +{offset} s: {line}")

    main = runs[0]
    with (args.out / "bariloche_runs.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        fields = ["changes", "bad", "increases", "breaks", "hypatia_changes", "drop_steps",
                  "median_ms", "min_ms", "max_ms", "median_laser_hops"]  # fmt: skip
        w.writerow(["start_offset_s", "strategy", *fields, "drop_causes"])
        for offset, by_strategy in runs.items():
            for s, r in by_strategy.items():
                causes = " ".join(f"{k}:{v}" for k, v in sorted(r["drop_causes"].items()))
                w.writerow([offset, s, *(r[x] for x in fields), causes])

    DOCS_DATA.mkdir(parents=True, exist_ok=True)
    with (DOCS_DATA / "bariloche_beijing.csv").open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(
            ["strategy", "changes", "published_changes", "difference", "bad", "published_bad",
             "median_ms", "published_median_ms", "drop_steps", "changes_min_over_starts",
             "changes_max_over_starts", "bad_min_over_starts", "bad_max_over_starts"]
        )  # fmt: skip
        print("\n  strategy: ours (published), bad ours (published); spread over 6 starts")
        for s in STRATEGIES:
            pc, pb = BARILOCHE_PUBLISHED[s]
            r = main[s]
            spread_c = [runs[o][s]["changes"] for o in runs]
            spread_b = [runs[o][s]["bad"] for o in runs]
            w.writerow(
                [s, r["changes"], pc, f"{(r['changes'] - pc) / pc:+.3f}", r["bad"], pb,
                 f"{r['median_ms']:.3f}", "just under 89", r["drop_steps"], min(spread_c),
                 max(spread_c), min(spread_b), max(spread_b)]
            )  # fmt: skip
            print(
                f"  {s}: {r['changes']} ({pc}, {(r['changes'] - pc) / pc:+.0%}), bad "
                f"{r['bad']} ({pb}); median {r['median_ms']:.2f} ms; over the starts changes "
                f"{min(spread_c)} to {max(spread_c)}, bad {min(spread_b)} to {max(spread_b)}"
            )
    return 0


def main(argv: list[str]) -> int:
    st.forbid_network()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--routing", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=st.REPO_ROOT / "results/phase-3/validation")
    parser.add_argument("--ground", type=Path, default=st.REPO_ROOT / "data/ground/2026-11-19")
    parser.add_argument(
        "--hypatia-cities", type=Path, default=st.REPO_ROOT / "data/ground/hypatia/cities.json"
    )
    parser.add_argument("setup", choices=["hypatia", "bariloche"])
    args = parser.parse_args(argv)
    if not args.routing.is_file():
        print(f"error: {args.routing} not found", file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    return run_hypatia(args) if args.setup == "hypatia" else run_bariloche(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

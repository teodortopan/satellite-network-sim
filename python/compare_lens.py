#!/usr/bin/env python3
"""Compare LENS's measured dish-to-PoP round trips with the routing program's (spec 6.4).

Reported, not pass or fail. The measurement: LENS dish `victoria` (Victoria, BC; PoP Seattle,
router sttlwax1, which SpaceX's file maps to PoP code sea) pinging the first address of its
PoP every 10 ms (python/fetch_lens.py). The model: the routing program, realistic model, on
the committed snapshot of 2026-11-18 and the ground data of 2026-11-19, pattern three,
highest-elevation serving satellite kept for 15 s slots, lowest-delay re-planned every second,
from Victoria (48.4284, -123.3656; the dish is known only to its city) to an end placed at the
Seattle PoP's own coordinates, so that the route is dish -> satellite -> (lasers) ->
satellite -> gateway -> fibre to the home PoP, with no fibre after the PoP. Round trip = twice
the one-way delay, every 1 s. The program checks that Victoria's home PoP is sea; this script
stops if it is not.

Per UTC hour of the files (21:00 and 22:00 on 2026-11-18; LENS lists no 23:00 file that day):
  measured  minimum, 5th percentile, median, 95th percentile of the RTT of every answered
            ping, the share unanswered, and the 15 s pattern: the median RTT per second of
            the minute folded on 15 s (seconds 0-14 after :00, :15, :30, :45), and where the
            largest step between consecutive one-second medians falls on that grid.
  model     the same statistics of the program's round trip, the drop share, the changes of
            serving satellite and gateway, and where its jumps fall on the same 15 s grid.

The ping files are read in place (a .txt.tar.zst needs Python 3.14 or later; inside the CI
image, Python 3.12, decompress them first with `zstd -d -k`, which leaves a .txt.tar next to
each). Members are read, never extracted to disk. A ping line's time is when its answer
arrived; the send time is that minus the RTT.

Writes docs/phase-3-data/lens_victoria.csv and, under results/phase-3/validation/, the
program's CSV and summary.

Usage (inside the CI image):

    python3 python/compare_lens.py --routing build/validate/bin/routing
"""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import re
import subprocess
import sys
import tarfile
from pathlib import Path

import numpy as np

import snapshot_tools as st

DATE = "2026-11-18"
HOURS = (21, 22)
VICTORIA = {"name": "Victoria", "country": "CA", "region": "CA-BC", "geofeed_city": "Victoria",
            "lat": 48.4284, "lon": -123.3656}  # fmt: skip
SEATTLE_POP = {"name": "Seattle PoP", "country": "US", "region": "US-WA",
               "geofeed_city": "Seattle", "lat": 47.6038, "lon": -122.3301}  # fmt: skip
HOME_POP = "sea"
SLOT_S = 15
_PING = re.compile(r"^\[(\d+\.\d+)\] \d+ bytes from [^ ]+ icmp_seq=(\d+) ttl=\d+ time=([\d.]+) ms")
_TOTALS = re.compile(r"^(\d+) packets transmitted, (\d+) received")


def read_pings(path: Path) -> tuple[np.ndarray, np.ndarray, int]:
    """Send times (unix s) and RTTs (ms) of answered pings, and the number never answered
    (from ping's closing statistics: icmp_seq is 16 bits and wraps within the hour)."""
    with tarfile.open(path) as archive:
        members = [m for m in archive.getmembers() if m.isfile()]
        if len(members) != 1:
            raise SystemExit(f"error: {path} holds {len(members)} files, expected one")
        text = archive.extractfile(members[0]).read().decode("ascii", errors="replace")
    times, rtts, lost = [], [], None
    for line in text.splitlines():
        if m := _PING.match(line):
            rtt = float(m.group(3))
            times.append(float(m.group(1)) - rtt / 1e3)
            rtts.append(rtt)
        elif m := _TOTALS.match(line):
            lost = int(m.group(1)) - int(m.group(2))
    if lost is None:
        raise SystemExit(f"error: {path} has no ping statistics line")
    return np.array(times), np.array(rtts), lost


def ping_file(directory: Path, hour: int) -> Path:
    stamp = f"{DATE}-{hour:02d}-00-00.txt.tar"
    for suffix in ("", ".zst"):
        found = sorted(directory.glob(f"*{stamp}{suffix}"))
        if found:
            return found[0]
    raise SystemExit(f"error: no ping file for {DATE} {hour:02d}:00 in {directory}")


def fold(times: np.ndarray, values: np.ndarray) -> tuple[list[float], int, float]:
    """Median per second of the 15 s grid; the grid second of the largest step between
    consecutive one-second medians (median over the hour of each second's step)."""
    second = np.floor(times).astype(np.int64)
    phase = second % SLOT_S
    per_phase = [float(np.median(values[phase == p])) for p in range(SLOT_S)]
    seconds = np.unique(second)
    medians = np.array([np.median(values[second == s]) for s in seconds])
    steps = np.abs(np.diff(medians))
    step_phase = seconds[1:] % SLOT_S
    by_phase = [float(np.median(steps[step_phase == p])) for p in range(SLOT_S)]
    return per_phase, int(np.argmax(by_phase)), max(by_phase)


def stats(values: np.ndarray) -> dict:
    return {
        "min_ms": float(values.min()),
        "p5_ms": float(np.percentile(values, 5)),
        "median_ms": float(np.median(values)),
        "p95_ms": float(np.percentile(values, 95)),
    }


def run_program(args) -> list[dict]:
    cities = args.out / "lens-cities.json"
    with cities.open("w", encoding="utf-8") as f:
        json.dump(
            {
                "cities": [VICTORIA, SEATTLE_POP],
                "pairs": [{"from": "Victoria", "to": "Seattle PoP", "role": "validation"}],
            },
            f,
        )
    out = args.out / "lens-victoria.csv"
    command = [
        str(args.routing),
        "--snapshot",
        str(args.snapshot),
        "--ground",
        str(args.ground),
        "--cities",
        str(cities),
        "--start",
        f"{DATE}T{HOURS[0]:02d}:00:00",
        "--duration-s",
        str(3600 * len(HOURS) - 1),
        "--step-s",
        "1",
        "--pattern",
        "three",
        "--model",
        "realistic",
        "--strategies",
        "lowest-delay",
        "--replan-s",
        "1",
        "--directions",
        "forward",
        "--out",
        str(out),
    ]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    (args.out / "lens-victoria.summary.txt").write_text(result.stdout, encoding="utf-8")
    if result.returncode != 0 or "checks: PASSED" not in result.stdout:
        raise SystemExit(f"error: routing exited with {result.returncode}\n{result.stderr}")
    if f"Victoria: {HOME_POP} " not in result.stdout:
        raise SystemExit(f"error: Victoria's home PoP is not {HOME_POP}; see the summary")
    with out.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def main(argv: list[str]) -> int:
    st.forbid_network()
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--routing", type=Path, required=True)
    parser.add_argument("--snapshot", type=Path, default=st.DEFAULT_SNAPSHOT)
    parser.add_argument("--ground", type=Path, default=st.REPO_ROOT / "data/ground/2026-11-19")
    parser.add_argument("--lens", type=Path, default=st.REPO_ROOT / f"data/lens/{DATE}/victoria")
    parser.add_argument("--out", type=Path, default=st.REPO_ROOT / "results/phase-3/validation")
    args = parser.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)

    rows = run_program(args)
    unix0 = st.parse_utc(f"{DATE}T00:00:00").unix_ns // st.NS_PER_S
    model_t = np.array([st.parse_utc(r["utc"]).unix_ns / st.NS_PER_S for r in rows])
    ok = np.array([r["status"] == "ok" for r in rows])
    model_rtt = np.array(
        [2.0 * float(r["one_way_ms"]) if r["one_way_ms"] else np.nan for r in rows]
    )

    table = []
    for hour in HOURS:
        begin = unix0 + 3600 * hour
        times, rtts, lost = read_pings(ping_file(args.lens, hour))
        keep = (times >= begin) & (times < begin + 3600)
        times, rtts = times[keep], rtts[keep]
        measured = stats(rtts)
        m_fold, m_phase, m_step = fold(times, rtts)
        in_hour = (model_t >= begin) & (model_t < begin + 3600)
        good = in_hour & ok
        model = stats(model_rtt[good])
        hour_rows = [r for r, x in zip(rows, in_hour, strict=True) if x]
        serving_phases = [
            round(model_t[i + 1]) % SLOT_S
            for i in np.flatnonzero(in_hour[:-1])
            if rows[i]["serving_norad_cat_id"] != rows[i + 1]["serving_norad_cat_id"]
        ]
        serving = len(serving_phases)
        gateways = sum(
            1
            for a, b in itertools.pairwise(hour_rows)
            if a["gateway"] and b["gateway"] and a["gateway"] != b["gateway"]
        )
        _, j_phase, j_step = fold(model_t[good], model_rtt[good])
        both = np.flatnonzero(in_hour[:-1] & ok[:-1] & ok[1:])
        largest_jump = float(np.max(np.abs(model_rtt[both + 1] - model_rtt[both])))
        print(
            f"{DATE} {hour:02d}:00 UTC\n"
            f"  measured: {len(rtts)} answered pings, {lost} unanswered; RTT min "
            f"{measured['min_ms']:.1f}, p5 {measured['p5_ms']:.1f}, median "
            f"{measured['median_ms']:.1f}, p95 {measured['p95_ms']:.1f} ms\n"
            f"    median RTT by second of the 15 s grid: "
            f"{' '.join(f'{x:.1f}' for x in m_fold)}\n"
            f"    largest step between one-second medians at grid second {m_phase} "
            f"(median {m_step:.1f} ms)\n"
            f"  model: {int(good.sum())} of {int(in_hour.sum())} steps with a route; RTT min "
            f"{model['min_ms']:.1f}, p5 {model['p5_ms']:.1f}, median {model['median_ms']:.1f}, "
            f"p95 {model['p95_ms']:.1f} ms; serving changes {serving} (grid seconds "
            f"{sorted(set(serving_phases))}), gateway changes {gateways}; largest step at grid "
            f"second {j_phase} (median {j_step:.2f} ms), largest jump {largest_jump:.2f} ms"
        )
        table.append(
            {
                "hour_utc": f"{DATE}T{hour:02d}:00",
                **{f"measured_{k}": f"{v:.2f}" for k, v in measured.items()},
                "measured_answered": len(rtts),
                "measured_unanswered": lost,
                "measured_largest_step_grid_second": m_phase,
                "measured_largest_step_median_ms": f"{m_step:.2f}",
                **{f"model_{k}": f"{v:.2f}" for k, v in model.items()},
                "model_steps_with_route": int(good.sum()),
                "model_steps": int(in_hour.sum()),
                "model_serving_changes": serving,
                "model_serving_change_grid_seconds": " ".join(
                    str(p) for p in sorted(set(serving_phases))
                ),
                "model_gateway_changes": gateways,
                "model_largest_step_grid_second": j_phase,
                "model_largest_step_median_ms": f"{j_step:.2f}",
                "model_largest_jump_ms": f"{largest_jump:.2f}",
            }
        )
    path = st.REPO_ROOT / "docs/phase-3-data/lens_victoria.csv"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(table[0]), lineterminator="\n")
        w.writeheader()
        w.writerows(table)
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

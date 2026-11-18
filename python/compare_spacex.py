#!/usr/bin/env python3
"""Measure how far our J2000 positions are from SpaceX's own predicted trajectories.

For every trajectory file of the snapshot (150 satellites), runs the C++ `propagate` program
over the file's whole time span at its 60 s steps, twice: once with the snapshot as it is
(every sampled satellite has a supplemental element set, fitted by CelesTrak to SpaceX's
trajectories) and once with a copy of the snapshot whose supgp.json holds no records, so the
program falls back to the standard element sets (fitted to US Space Force radar tracking).
At every step the difference (ours minus SpaceX) is split into radial, in-track and
cross-track parts, using the SpaceX position and velocity to define the directions, and set
against element-set age (time minus the element set's epoch; negative before the epoch).

This is a measurement, not a pass/fail check, with one alarm: if the median difference within
30 minutes of the epoch exceeds 10 km for supplemental element sets, that is a bug in frames or
time, not the model's error, and the script exits 1.

The trajectory files are not in git (see .gitignore); metadata.json lists their sha256, and
every file is checked against it before use. Run by hand, never by CI. Reads only local
files, and writes only to a temporary directory.

Trajectory file format ("Modified ITC", SpaceX's space-safety documentation): line 1
`created:<UTC>`, line 2 `ephemeris_start:<UTC> ephemeris_stop:<UTC> step_size:<s>`, line 3
`ephemeris_source:<text>`, line 4 the covariance frame (`UVW`), then per step four lines:
`yyyyDDDhhmmss.sss x y z vx vy vz` (UTC; km and km/s; frame MEME, the mean equator and equinox
of J2000) and three lines holding 7 + 7 + 7 = 21 values, the lower triangle of the 6x6
position-velocity covariance, row by row (C11; C21 C22; C31 C32 C33; ...; C61 ... C66), in
the satellite's UVW frame (U radial, V in-track, W cross-track). The documentation gives the
layout but not the units; they are km^2, km^2/s and km^2/s^2, the units of the state.
Checked on every 50th record of the snapshot's files (docs/phase-1-results.md): read this way
every matrix is positive semi-definite (read as an upper triangle, none is), the
velocity-to-position sigma ratio is 1.0e-3 to 1.9e-3 per second (the orbital rate, as for a
consistent unit), and the values SpaceX writes beyond its forecast horizon are exactly 0.01,
6.25 and 0.000225 (and 0.09, 14.44, 0.25), i.e. round sigmas of 100 m, 2500 m and 15 m (and
300, 3800, 500 m) only if the unit is km^2. Only the position block (U, V, W variances) is
used here.

Usage (inside the CI image):

    python3 python/compare_spacex.py --propagate build/validate/bin/propagate \\
        --ephemerides /path/to/data/starlink/2026-11-18/ephemerides
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import itertools
import json
import os
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np

import snapshot_tools as st

STEP_NS = 60 * st.NS_PER_S
# Element-age bins in hours. The bin around zero is the "near the epoch" value.
AGE_BIN_EDGES_H = (-24, -12, -6, -3, -1, -0.5, 0.5, 1, 3, 6, 12, 24, 36, 48, 72)
ALARM_WINDOW_H = 0.5
ALARM_MEDIAN_KM = 10.0
KINDS = ("supplemental", "standard")


@dataclass
class Trajectory:
    norad_cat_id: int
    file: str
    created: str
    times: list[st.UtcInstant]
    position_km: np.ndarray  # (n, 3), J2000 (MEME)
    velocity_km_s: np.ndarray  # (n, 3)
    sigma_uvw_km: np.ndarray  # (n, 3), 1-sigma radial, in-track, cross-track


def parse_trajectory_time(text: str) -> st.UtcInstant:
    """yyyyDDDhhmmss.sss (day of year), UTC, to the millisecond."""
    year, day_of_year = int(text[0:4]), int(text[4:7])
    hour, minute = int(text[7:9]), int(text[9:11])
    whole_s, _, millis = text[11:].partition(".")
    date = datetime.date(year, 1, 1) + datetime.timedelta(days=day_of_year - 1)
    second_ns = int(whole_s) * st.NS_PER_S + int(millis.ljust(3, "0")[:3]) * 1_000_000
    return st.parse_utc(f"{date.isoformat()}T{hour:02d}:{minute:02d}:00").plus_ns(second_ns)


def read_trajectory(path: Path, norad_cat_id: int) -> Trajectory:
    lines = path.read_text(encoding="ascii").splitlines()
    if len(lines) < 8 or not lines[0].startswith("created:") or lines[3].strip() != "UVW":
        raise SystemExit(f"error: {path} is not a trajectory file with a UVW covariance")
    body = [line for line in lines[4:] if line.strip()]
    if len(body) % 4:
        raise SystemExit(f"error: {path}: {len(body)} data lines, not a multiple of 4")
    times, states, sigmas = [], [], []
    for k in range(0, len(body), 4):
        fields = body[k].split()
        covariance = [float(v) for v in " ".join(body[k + 1 : k + 4]).split()]
        if len(fields) != 7 or len(covariance) != 21:
            raise SystemExit(f"error: {path}: malformed record at data line {k + 1}")
        times.append(parse_trajectory_time(fields[0]))
        states.append([float(v) for v in fields[1:]])
        # Diagonal of the position block in the row-by-row lower triangle: C11, C22, C33.
        sigmas.append([covariance[0], covariance[2], covariance[5]])
    variances = np.array(sigmas)
    if (variances < 0).any():
        raise SystemExit(f"error: {path}: negative variance")
    state = np.array(states)
    return Trajectory(
        norad_cat_id=norad_cat_id,
        file=path.name,
        created=lines[0].removeprefix("created:").strip(),
        times=times,
        position_km=state[:, :3],
        velocity_km_s=state[:, 3:],
        sigma_uvw_km=np.sqrt(variances),
    )


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_trajectories(snapshot: Path, ephemerides: Path) -> list[Trajectory]:
    """Every trajectory file metadata.json lists, each checked against its sha256."""
    if not ephemerides.is_dir():
        raise SystemExit(
            f"error: trajectory directory {ephemerides} not found. The files are not in git; "
            "they exist only on the machine that downloaded the snapshot. Pass "
            "--ephemerides <dir>."
        )
    metadata = json.loads((snapshot / "metadata.json").read_text(encoding="utf-8"))
    entries = metadata["ephemerides"]["files"]
    trajectories = []
    for entry in entries:
        path = ephemerides / entry["file"]
        if not path.is_file():
            raise SystemExit(f"error: {path} (listed in metadata.json) not found")
        if sha256_of(path) != entry["sha256"]:
            raise SystemExit(f"error: {path}: sha256 differs from metadata.json")
        trajectories.append(read_trajectory(path, int(entry["norad_cat_id"])))
    print(f"{len(trajectories)} trajectory files, sha256 verified against metadata.json")
    return trajectories


def standard_only_snapshot(snapshot: Path, work: Path) -> Path:
    """A copy of the snapshot whose supgp.json holds no records (links to the other files)."""
    directory = work / "standard-only"
    directory.mkdir()
    for name in (st.STANDARD_FILE, st.EOP_FILE):
        os.symlink((snapshot / name).resolve(), directory / name)
    (directory / st.SUPPLEMENTAL_FILE).write_text("[]\n", encoding="utf-8")
    return directory


@dataclass
class Differences:
    """One entry per compared step, for one element-set kind."""

    age_h: np.ndarray
    total_km: np.ndarray
    radial_km: np.ndarray  # Signed, ours minus SpaceX.
    in_track_km: np.ndarray
    cross_track_km: np.ndarray
    sigma_km: np.ndarray  # SpaceX's 1-sigma position uncertainty, root-sum-square of U, V, W.
    sigma_uvw_km: np.ndarray  # (n, 3)
    epochs: dict[int, st.UtcInstant]


def compare_one(trajectory: Trajectory, rows: list[dict], kind: str) -> dict:
    by_time = {st.parse_utc(row["utc"]).unix_ns: row for row in rows}
    ages, ours, keep = [], [], []
    for i, instant in enumerate(trajectory.times):
        row = by_time.get(instant.unix_ns)
        if row is None:
            continue  # SGP4 failed at this time; the program counted it.
        if row["element_source"] != kind:
            raise SystemExit(
                f"error: {trajectory.norad_cat_id} used {row['element_source']} elements in "
                f"the {kind} run"
            )
        ages.append(row["element_age_s"] / 3600.0)
        ours.append([row["j2000_x_km"], row["j2000_y_km"], row["j2000_z_km"]])
        keep.append(i)
    r = trajectory.position_km[keep]
    v = trajectory.velocity_km_s[keep]
    d = np.array(ours) - r
    radial = r / np.linalg.norm(r, axis=1)[:, None]
    normal = np.cross(r, v)
    cross = normal / np.linalg.norm(normal, axis=1)[:, None]
    in_track = np.cross(cross, radial)
    sigma_uvw = trajectory.sigma_uvw_km[keep]
    return {
        "age_h": np.array(ages),
        "total_km": np.linalg.norm(d, axis=1),
        "radial_km": np.einsum("ij,ij->i", d, radial),
        "in_track_km": np.einsum("ij,ij->i", d, in_track),
        "cross_track_km": np.einsum("ij,ij->i", d, cross),
        "sigma_km": np.linalg.norm(sigma_uvw, axis=1),
        "sigma_uvw_km": sigma_uvw,
    }


def measure(exe: Path, snapshot: Path, trajectories: list[Trajectory]) -> dict[str, Differences]:
    chosen_sup = st.load_snapshot_elements(snapshot)
    results: dict[str, Differences] = {}
    with tempfile.TemporaryDirectory(prefix="satsim-spacex-") as work_dir:
        work = Path(work_dir)
        snapshots = {"supplemental": snapshot, "standard": standard_only_snapshot(snapshot, work)}
        chosen = {
            "supplemental": chosen_sup,
            "standard": st.load_snapshot_elements(snapshots["standard"]),
        }
        for kind in KINDS:
            parts = []
            epochs = {}
            for trajectory in trajectories:
                number = trajectory.norad_cat_id
                if number not in chosen[kind] or chosen[kind][number].source != kind:
                    print(f"  {number}: no {kind} element set; skipped")
                    continue
                epochs[number] = chosen[kind][number].epoch
                steps = {b.unix_ns - a.unix_ns for a, b in itertools.pairwise(trajectory.times)}
                if steps != {STEP_NS}:
                    raise SystemExit(f"error: {trajectory.file}: steps are not all 60 s")
                csv_path = work / f"{kind}-{number}.csv"
                st.run_propagate(
                    exe,
                    snapshots[kind],
                    trajectory.times[0],
                    trajectory.times[-1].unix_ns - trajectory.times[0].unix_ns,
                    STEP_NS,
                    [number],
                    csv_path,
                )
                parts.append(compare_one(trajectory, st.read_propagate_csv(csv_path), kind))
                csv_path.unlink()
            results[kind] = Differences(
                **{key: np.concatenate([p[key] for p in parts]) for key in parts[0]},
                epochs=epochs,
            )
    return results


def km(value: float) -> str:
    return f"{value:.3g}"


def print_table(kind: str, diff: Differences) -> None:
    print(f"\n{kind} element sets: {len(diff.epochs)} satellites, {len(diff.age_h)} steps")
    print(
        "| age (h) | steps | median | p95 | radial med / p95 | in-track med / p95 | "
        "cross-track med / p95 | SpaceX 1-sigma med (radial, in-track, cross) |"
    )
    print("|---|---|---|---|---|---|---|---|")
    for low, high in itertools.pairwise(AGE_BIN_EDGES_H):
        mask = (diff.age_h >= low) & (diff.age_h < high)
        if not mask.any():
            continue

        def mp(values, mask=mask):
            a = np.abs(values[mask])
            return f"{km(np.median(a))} / {km(np.percentile(a, 95))}"

        sigma = np.median(diff.sigma_uvw_km[mask], axis=0)
        print(
            f"| [{low}, {high}) | {mask.sum()} | {km(np.median(diff.total_km[mask]))} | "
            f"{km(np.percentile(diff.total_km[mask], 95))} | {mp(diff.radial_km)} | "
            f"{mp(diff.in_track_km)} | {mp(diff.cross_track_km)} | "
            f"{km(np.median(diff.sigma_km[mask]))} ({km(sigma[0])}, {km(sigma[1])}, "
            f"{km(sigma[2])}) |"
        )
    print("(all distances in km; components are absolute values of ours minus SpaceX)")


def near_epoch_median_km(diff: Differences) -> float:
    mask = np.abs(diff.age_h) <= ALARM_WINDOW_H
    return float(np.median(diff.total_km[mask])) if mask.any() else float("nan")


def print_caveats(trajectories: list[Trajectory], results: dict[str, Differences]) -> None:
    created = sorted(t.created for t in trajectories)
    print("\ndata facts:")
    print(f"  trajectory files created {created[0]} to {created[-1]}")
    spans = sorted(
        {round((t.times[-1].unix_ns - t.times[0].unix_ns) / 3.6e12) for t in trajectories}
    )
    print(f"  trajectory spans (h): {spans}")
    for kind in KINDS:
        epochs = sorted(results[kind].epochs.values(), key=lambda e: e.unix_ns)
        if not epochs:
            continue
        later = sum(
            1
            for t in trajectories
            if t.norad_cat_id in results[kind].epochs
            and results[kind].epochs[t.norad_cat_id].unix_ns
            > st.parse_utc(t.created.replace(" UTC", "").replace(" ", "T")).unix_ns
        )
        print(
            f"  {kind} epochs {epochs[0].iso()} to {epochs[-1].iso()}; "
            f"{later} of {len(epochs)} are later than their trajectory file's creation"
        )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare propagate's J2000 positions with SpaceX's trajectory files."
    )
    parser.add_argument("--propagate", type=Path, required=True, help="built propagate program")
    parser.add_argument(
        "--snapshot",
        type=Path,
        default=st.DEFAULT_SNAPSHOT,
        help=f"snapshot directory (default: {st.DEFAULT_SNAPSHOT.relative_to(st.REPO_ROOT)})",
    )
    parser.add_argument(
        "--ephemerides",
        type=Path,
        help="directory of the snapshot's trajectory files (default: <snapshot>/ephemerides)",
    )
    args = parser.parse_args(argv)
    if args.ephemerides is None:
        args.ephemerides = args.snapshot / "ephemerides"
    return args


def main(argv: list[str]) -> int:
    st.forbid_network()
    args = parse_args(argv)
    st.check_snapshot(args.snapshot)
    if not args.propagate.is_file():
        print(f"error: {args.propagate} not found", file=sys.stderr)
        return 2
    trajectories = load_trajectories(args.snapshot, args.ephemerides)
    results = measure(args.propagate, args.snapshot, trajectories)
    for kind in KINDS:
        print_table(kind, results[kind])
    print_caveats(trajectories, results)
    print()
    status = 0
    for kind in KINDS:
        value = near_epoch_median_km(results[kind])
        print(f"median difference within {ALARM_WINDOW_H} h of the epoch, {kind}: {km(value)} km")
    if not near_epoch_median_km(results["supplemental"]) <= ALARM_MEDIAN_KM:
        print(
            f"WARNING: the supplemental median within {ALARM_WINDOW_H} h of the epoch exceeds "
            f"{ALARM_MEDIAN_KM} km (or there are no such steps). SGP4 alone does not explain "
            "that: look for a frame or time error.",
            file=sys.stderr,
        )
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Regenerate the Phase 1 plots in docs/img/phase-1/ from the propagate program's output.

  ground_track.png       one satellite's ground track over three orbits (committed snapshot)
  altitude.png           the constellation's height distribution at the snapshot time
                         (committed snapshot)
  skyfield_agreement.png median and worst difference from Skyfield per quantity, against
                         the tolerances (committed snapshot; runs the validate sample)
  spacex_difference.png  difference from SpaceX's trajectories against element-set age, for
                         supplemental and standard element sets, with SpaceX's stated 1-sigma
                         uncertainty. Needs the snapshot's trajectory files, which are not in
                         git: skipped (with a message) when --ephemerides is not given.

Every number is computed again by running propagate; nothing is read from a saved result.
Writes only the PNG files. Reads only local files.

Usage (inside the CI image):

    python3 python/plot_phase1.py --propagate build/validate/bin/propagate \\
        [--ephemerides /path/to/data/starlink/2026-11-18/ephemerides]
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import compare_spacex
import snapshot_tools as st
import validate_against_reference as validate

DEFAULT_OUT = st.REPO_ROOT / "docs" / "img" / "phase-1"
SNAPSHOT_TIME = "2026-11-18T21:15:00"
GROUND_TRACK_ID = 56882  # STARLINK-6175, inclination 43 degrees, near 485 km.
GROUND_TRACK_MINUTES = 290  # A little over three orbits of about 95 minutes.

# Opaque light surface with dark ink, so the images read the same in GitHub's light and dark
# themes. Categorical colours from a palette validated for colour-vision deficiency.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#dcdbd7"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
AQUA = "#1baf7a"
GRAY = "#7a7974"

plt.rcParams.update(
    {
        "figure.facecolor": SURFACE,
        "axes.facecolor": SURFACE,
        "savefig.facecolor": SURFACE,
        "text.color": INK,
        "axes.labelcolor": INK,
        "axes.edgecolor": INK_SECONDARY,
        "xtick.color": INK_SECONDARY,
        "ytick.color": INK_SECONDARY,
        "axes.grid": True,
        "grid.color": GRID,
        "grid.linewidth": 0.6,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "font.size": 10,
        "axes.titlesize": 11,
        "legend.frameon": False,
        "lines.linewidth": 2.0,
    }
)


def save(fig, out: Path, name: str) -> None:
    fig.tight_layout()
    path = out / name
    fig.savefig(path, dpi=110)
    plt.close(fig)
    print(f"wrote {path}")


def ground_track(exe: Path, snapshot: Path, work: Path, out: Path) -> None:
    csv_path = work / "track.csv"
    st.run_propagate(
        exe,
        snapshot,
        st.parse_utc(SNAPSHOT_TIME),
        GROUND_TRACK_MINUTES * 60 * st.NS_PER_S,
        30 * st.NS_PER_S,
        [GROUND_TRACK_ID],
        csv_path,
    )
    rows = st.read_propagate_csv(csv_path)
    lon = np.array([r["geodetic_longitude_deg"] for r in rows])
    lat = np.array([r["geodetic_latitude_deg"] for r in rows])
    # Break the line where it wraps from +180 to -180 degrees.
    breaks = np.where(np.abs(np.diff(lon)) > 180)[0] + 1
    fig, ax = plt.subplots(figsize=(8, 4.2))
    for segment_lon, segment_lat in zip(np.split(lon, breaks), np.split(lat, breaks), strict=True):
        ax.plot(segment_lon, segment_lat, color=BLUE)
    ax.plot(lon[0], lat[0], "o", color=BLUE, markersize=8)
    ax.annotate(
        f"start {SNAPSHOT_TIME} UTC",
        (lon[0], lat[0]),
        xytext=(8, 8),
        textcoords="offset points",
        color=INK_SECONDARY,
    )
    ax.set_xlim(-180, 180)
    ax.set_ylim(-90, 90)
    ax.set_xticks(range(-180, 181, 60))
    ax.set_yticks(range(-90, 91, 30))
    ax.set_xlabel("geodetic longitude (degrees east)")
    ax.set_ylabel("geodetic latitude (degrees)")
    ax.set_title(
        f"Ground track of {rows[0]['object_name']} ({GROUND_TRACK_ID}), "
        f"{GROUND_TRACK_MINUTES} minutes (about three orbits)"
    )
    save(fig, out, "ground_track.png")


def altitude(exe: Path, snapshot: Path, work: Path, out: Path) -> None:
    csv_path = work / "all.csv"
    st.run_propagate(exe, snapshot, st.parse_utc(SNAPSHOT_TIME), 0, 60 * st.NS_PER_S, [], csv_path)
    heights = np.array([r["geodetic_height_km"] for r in st.read_propagate_csv(csv_path)])
    fig, ax = plt.subplots(figsize=(8, 4.2))
    ax.hist(heights, bins=np.arange(100, 625, 5), color=BLUE, edgecolor=SURFACE, linewidth=0.5)
    ax.set_yscale("log")
    ax.set_xlabel("geodetic height above the WGS-84 ellipsoid (km)")
    ax.set_ylabel("satellites per 5 km (log scale)")
    ax.set_title(f"Height of all {len(heights)} satellites at {SNAPSHOT_TIME} UTC")
    save(fig, out, "altitude.png")


def skyfield_agreement(exe: Path, snapshot: Path, work: Path, out: Path) -> None:
    chosen = st.load_snapshot_elements(snapshot)
    ids = validate.choose_sample(chosen)
    times = validate.sample_times()
    csv_path = work / "sample.csv"
    st.run_propagate(
        exe, snapshot, times[0], validate.SAMPLE_DURATION_NS, validate.SAMPLE_STEP_NS, ids, csv_path
    )
    outcome = validate.compare(st.read_propagate_csv(csv_path), snapshot, ids, times)
    panels = (
        (
            "position (m)",
            [
                ("TEME", "teme_position_m"),
                ("ECEF (ITRF)", "ecef_position_m"),
                ("latitude", "latitude_m"),
                ("longitude", "longitude_m"),
                ("height", "height_m"),
                ("J2000 vs GCRS", "j2000_position_m"),
            ],
        ),
        (
            "velocity (mm/s)",
            [("TEME", "teme_velocity_mm_s"), ("J2000 vs GCRS", "j2000_velocity_mm_s")],
        ),
    )
    fig, axes = plt.subplots(
        2, 1, figsize=(8, 5.2), gridspec_kw={"height_ratios": [6, 2.6]}, sharex=False
    )
    for ax, (unit, quantities) in zip(axes, panels, strict=True):
        labels = []
        for y, (label, key) in enumerate(reversed(quantities)):
            values = np.array([v for v, _, _ in outcome.values[key]])
            # A difference of exactly zero cannot be drawn on a log axis.
            floor = 1e-12
            ax.plot(max(np.median(values), floor), y, "o", color=BLUE, markersize=8)
            ax.plot(max(values.max(), floor), y, "D", color=ORANGE, markersize=8)
            ax.plot(validate.TOLERANCES[key], y, "|", color=INK, markersize=16, mew=2)
            labels.append(label)
        ax.set_yticks(range(len(labels)), labels)
        ax.set_xscale("log")
        ax.set_xlim(1e-9, 1e2)
        ax.set_ylim(-0.6, len(labels) - 0.4)
        ax.set_xlabel(f"difference from Skyfield, {unit} (log scale)")
        ax.grid(axis="y", visible=False)
    axes[0].plot([], [], "o", color=BLUE, label="median")
    axes[0].plot([], [], "D", color=ORANGE, label="worst")
    axes[0].plot([], [], "|", color=INK, markersize=12, mew=2, label="tolerance")
    axes[0].legend(loc="upper right")
    axes[0].set_title(
        f"Agreement with Skyfield: {outcome.satellites} satellites, "
        f"{len(outcome.values['teme_position_m'])} rows"
    )
    save(fig, out, "skyfield_agreement.png")


def spacex_difference(exe: Path, snapshot: Path, ephemerides: Path, out: Path) -> None:
    trajectories = compare_spacex.load_trajectories(snapshot, ephemerides)
    results = compare_spacex.measure(exe, snapshot, trajectories)
    edges = np.arange(-12, 73, 1.0)
    centres = (edges[:-1] + edges[1:]) / 2
    fig, ax = plt.subplots(figsize=(8, 4.8))
    sigma = None
    for kind, colour in (("supplemental", BLUE), ("standard", ORANGE)):
        diff = results[kind]
        index = np.digitize(diff.age_h, edges) - 1
        median, p95, sig = [], [], []
        for b in range(len(centres)):
            mask = index == b
            enough = mask.sum() >= 30
            median.append(np.median(diff.total_km[mask]) if enough else np.nan)
            p95.append(np.percentile(diff.total_km[mask], 95) if enough else np.nan)
            sig.append(np.median(diff.sigma_km[mask]) if enough else np.nan)
        ax.plot(centres, median, color=colour, label=f"{kind}: median")
        ax.plot(
            centres, p95, color=colour, linewidth=1.2, linestyle="--", label=f"{kind}: 95th pct"
        )
        if kind == "supplemental":
            sigma = sig
    ax.plot(centres, sigma, color=GRAY, linewidth=1.5, linestyle=":", label="SpaceX stated 1-sigma")
    ax.axvline(0, color=INK_SECONDARY, linewidth=0.8)
    ax.set_yscale("log")
    ax.set_xlim(-12, 72)
    ax.set_xticks(range(-12, 73, 12))
    ax.set_xlabel("element-set age: time minus element epoch (hours)")
    ax.set_ylabel("position difference from SpaceX (km, log scale)")
    ax.set_title("Our J2000 positions against SpaceX's predicted trajectories (150 satellites)")
    ax.legend(loc="upper left", fontsize=9)
    save(fig, out, "spacex_difference.png")


def main(argv: list[str]) -> int:
    st.forbid_network()
    parser = argparse.ArgumentParser(description="Regenerate the Phase 1 plots.")
    parser.add_argument("--propagate", type=Path, required=True, help="built propagate program")
    parser.add_argument("--snapshot", type=Path, default=st.DEFAULT_SNAPSHOT)
    parser.add_argument("--ephemerides", type=Path, help="trajectory files (SpaceX plot only)")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output directory")
    args = parser.parse_args(argv)
    st.check_snapshot(args.snapshot)
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="satsim-plots-") as work_dir:
        work = Path(work_dir)
        ground_track(args.propagate, args.snapshot, work, args.out)
        altitude(args.propagate, args.snapshot, work, args.out)
        skyfield_agreement(args.propagate, args.snapshot, work, args.out)
    if args.ephemerides is None:
        print("spacex_difference.png skipped: pass --ephemerides (the files are not in git)")
    else:
        spacex_difference(args.propagate, args.snapshot, args.ephemerides, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

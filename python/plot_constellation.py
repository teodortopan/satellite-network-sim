#!/usr/bin/env python3
"""The shells-and-planes study: evidence for the constellation program's proposed thresholds.

Runs the constellation program on a snapshot at one reference time, reads its CSV and its
summary (which states the shell definitions in use), and writes to docs/img/phase-2/:

  heights.png            per working shell: mean height of the satellites in its inclination
                         band, with the proposed height band
  inclinations.png       per working shell: inclination of the satellites in its height band,
                         with the proposed inclination band
  raan.png               per working shell: RAAN against argument of latitude at the common
                         reference time; each plane is a column, each satellite a dot
  threshold.png          per working shell: planes and satellites in planes against the plane
                         gap threshold, with the proposed threshold
  plane_sizes.png        per working shell: plane sizes at the proposed threshold
  spacing.png            per working shell: gaps between neighbours along the orbit
  shell53.png            the 53 degree investigation: height against RAAN drift rate, and
                         planes against threshold for raw element RAAN, one wide height band,
                         the proposed narrow band, and a split by drift rate
  tables.txt             the numbers behind the plots, as plain-text tables

Clustering here is written again from the program's documented rule (cut the RAAN circle at
gaps above the threshold, drop groups smaller than the minimum plane size), to sweep
thresholds; at the proposed thresholds it must reproduce the program's planes, which the
script checks. Reads only local files.

Usage (inside the CI image):

    python3 python/plot_constellation.py --constellation build/validate/bin/constellation
"""

from __future__ import annotations

import argparse
import csv
import io
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import snapshot_tools as st

DEFAULT_OUT = st.REPO_ROOT / "docs" / "img" / "phase-2"
REFERENCE_TIME = "2026-11-18T21:15:00"
THRESHOLDS_DEG = np.geomspace(0.05, 6.0, 49)
TABLE_THRESHOLDS_DEG = (0.1, 0.2, 0.3, 0.4, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0)
# Satellites at the same inclination but outside the shell band, for the height histograms.
HEIGHT_VIEW_MARGIN_KM = 15.0

# The same light surface and palette as plot_phase1.py.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#dcdbd7"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
AQUA = "#1baf7a"
GRAY = "#7a7974"
BAND = "#2a78d61f"

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
        "font.size": 9,
        "axes.titlesize": 10,
        "legend.frameon": False,
        "lines.linewidth": 1.8,
    }
)

_SHELL_LINE = re.compile(
    r"^  shell (?P<name>\S+) \(inclination (?P<inc>[\d.]+) \+- (?P<tol>[\d.]+) deg, height "
    r"(?P<hmin>[\d.]+) to (?P<hmax>[\d.]+) km, plane gap (?P<gap>[\d.]+) deg, planes of "
    r"(?P<min_size>\d+) or more\): (?P<in_band>\d+) satellites in the bands, (?P<in_planes>\d+) "
    r"in planes, (?P<planes>\d+) planes"
)


@dataclass(frozen=True)
class Shell:
    name: str
    inclination_deg: float
    tolerance_deg: float
    min_height_km: float
    max_height_km: float
    gap_deg: float
    min_plane_size: int
    planes: int


@dataclass
class Sat:
    norad_cat_id: int
    object_id: str
    shell: str
    plane: int | None
    index_in_plane: int | None
    reason: str
    inc: float
    raan: float
    u: float
    height: float
    osc_inc: float
    osc_raan: float
    osc_u: float
    drift: float  # deg/day; nan when absent


def run_constellation(exe: Path, snapshot: Path, time: str, work: Path) -> tuple[Path, str]:
    csv_path = work / "constellation.csv"
    done = subprocess.run(
        [str(exe), "--snapshot", str(snapshot), "--time", time, "--out", str(csv_path)],
        check=True,
        capture_output=True,
        text=True,
    )
    return csv_path, done.stdout


def parse_shells(summary: str) -> list[Shell]:
    shells = []
    for line in summary.splitlines():
        m = _SHELL_LINE.match(line)
        if m:
            shells.append(
                Shell(
                    m["name"],
                    float(m["inc"]),
                    float(m["tol"]),
                    float(m["hmin"]),
                    float(m["hmax"]),
                    float(m["gap"]),
                    int(m["min_size"]),
                    int(m["planes"]),
                )
            )
    if not shells:
        raise SystemExit("error: no shell lines in the constellation summary")
    return shells


def number(text: str) -> float:
    return float(text) if text else float("nan")


def read_csv(path: Path) -> list[Sat]:
    sats = []
    with path.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if row["propagation_error"]:
                continue  # No geometry.
            sats.append(
                Sat(
                    int(row["norad_cat_id"]),
                    row["object_id"],
                    row["shell"],
                    int(row["plane_index"]) if row["plane_index"] else None,
                    int(row["index_in_plane"]) if row["index_in_plane"] else None,
                    row["exclusion_reason"],
                    float(row["inclination_deg"]),
                    float(row["raan_teme_deg"]),
                    float(row["argument_of_latitude_deg"]),
                    float(row["mean_height_km"]),
                    float(row["osculating_inclination_deg"]),
                    float(row["osculating_raan_teme_deg"]),
                    float(row["osculating_argument_of_latitude_deg"]),
                    number(row["raan_drift_deg_per_day"]),
                )
            )
    return sats


def clusters(raan_deg: np.ndarray, threshold_deg: float) -> list[np.ndarray]:
    """Groups of indices: the circle of RAAN cut at every gap above the threshold."""
    if len(raan_deg) == 0:
        return []
    order = np.argsort(raan_deg % 360.0, kind="stable")
    ordered = raan_deg[order] % 360.0
    gaps = np.diff(np.concatenate([ordered, [ordered[0] + 360.0]]))  # gap after each member
    cuts = np.flatnonzero(gaps > threshold_deg)
    if len(cuts) == 0:
        return [order]
    starts = (cuts + 1) % len(order)
    rolled = np.roll(order, -starts[0])
    split_at = np.sort((starts - starts[0]) % len(order))[1:]
    return np.split(rolled, split_at)


def plane_stats(raan_deg: np.ndarray, threshold_deg: float, min_size: int) -> dict:
    sizes = np.array([len(c) for c in clusters(raan_deg, threshold_deg)])
    kept = sizes[sizes >= min_size]
    return {
        "planes": len(kept),
        "in_planes": int(kept.sum()),
        "median": float(np.median(kept)) if len(kept) else 0.0,
        "max": int(kept.max()) if len(kept) else 0,
    }


def candidates(sats: list[Sat], shell: Shell) -> list[Sat]:
    """The satellites the program grouped into planes: in the shell, not excluded before."""
    return [s for s in sats if s.shell == shell.name and s.reason in ("", "no_plane")]


def in_plane_gaps(sats: list[Sat], shell: Shell) -> tuple[np.ndarray, list[tuple[float, int]]]:
    """Gaps between neighbours along the orbit in every plane, and the largest one per plane."""
    by_plane: dict[int, list[float]] = {}
    for s in sats:
        if s.shell == shell.name and s.plane is not None:
            by_plane.setdefault(s.plane, []).append(s.u)
    gaps = []
    largest = []
    for plane, us in sorted(by_plane.items()):
        u = np.sort(np.array(us))
        g = np.diff(np.concatenate([u, [u[0] + 360.0]]))
        gaps.extend(g)
        largest.append((float(g.max()), plane))
    return np.array(gaps), sorted(largest, reverse=True)


def save(fig, out: Path, name: str) -> None:
    fig.tight_layout()
    path = out / name
    fig.savefig(path, dpi=110)
    plt.close(fig)
    print(f"wrote {path}")


def panels(count: int, height: float = 3.0):
    fig, axes = plt.subplots(1, count, figsize=(3.6 * count, height))
    return fig, np.atleast_1d(axes)


def plot_heights(sats: list[Sat], shells: list[Shell], out: Path) -> None:
    fig, axes = panels(len(shells))
    for ax, shell in zip(axes, shells, strict=True):
        h = np.array(
            [s.height for s in sats if abs(s.inc - shell.inclination_deg) <= shell.tolerance_deg]
        )
        lo = shell.min_height_km - HEIGHT_VIEW_MARGIN_KM
        hi = shell.max_height_km + HEIGHT_VIEW_MARGIN_KM
        ax.axvspan(shell.min_height_km, shell.max_height_km, color=BAND, label="proposed band")
        ax.hist(h[(h >= lo) & (h < hi)], bins=np.arange(lo, hi + 0.25, 0.25), color=BLUE)
        ax.set_yscale("log")
        ax.set_xlabel("mean geodetic height (km)")
        ax.set_title(f"{shell.name}: inclination {shell.inclination_deg} +- {shell.tolerance_deg}")
    axes[0].set_ylabel("satellites per 0.25 km (log)")
    axes[0].legend(loc="upper left")
    save(fig, out, "heights.png")


def plot_inclinations(sats: list[Sat], shells: list[Shell], out: Path) -> None:
    fig, axes = panels(len(shells))
    for ax, shell in zip(axes, shells, strict=True):
        inc = np.array(
            [s.inc for s in sats if shell.min_height_km <= s.height < shell.max_height_km]
        )
        lo = shell.inclination_deg - 1.0
        hi = shell.inclination_deg + 1.0
        ax.axvspan(
            shell.inclination_deg - shell.tolerance_deg,
            shell.inclination_deg + shell.tolerance_deg,
            color=BAND,
            label="proposed band",
        )
        ax.hist(inc[(inc >= lo) & (inc < hi)], bins=np.arange(lo, hi + 0.01, 0.01), color=BLUE)
        ax.set_yscale("log")
        ax.set_xlabel("mean inclination (deg)")
        ax.set_title(f"{shell.name}: height {shell.min_height_km}-{shell.max_height_km} km")
    axes[0].set_ylabel("satellites per 0.01 deg (log)")
    axes[0].legend(loc="upper left")
    save(fig, out, "inclinations.png")


def plot_raan(sats: list[Sat], shells: list[Shell], out: Path) -> None:
    fig, axes = panels(len(shells), height=3.4)
    for ax, shell in zip(axes, shells, strict=True):
        members = candidates(sats, shell)
        placed = [s for s in members if s.plane is not None]
        dropped = [s for s in members if s.plane is None]
        ax.scatter(
            [s.raan for s in placed],
            [s.u for s in placed],
            s=1.5,
            color=BLUE,
            label="in a plane",
            linewidths=0,
        )
        ax.scatter(
            [s.raan for s in dropped],
            [s.u for s in dropped],
            s=4,
            color=ORANGE,
            label="no plane",
            linewidths=0,
        )
        ax.set_xlim(0, 360)
        ax.set_ylim(0, 360)
        ax.set_xticks(range(0, 361, 90))
        ax.set_yticks(range(0, 361, 90))
        ax.set_xlabel("RAAN at the reference time (deg)")
        ax.set_title(f"{shell.name}: {len(placed)} in {shell.planes} planes")
    axes[0].set_ylabel("argument of latitude (deg)")
    axes[0].legend(loc="upper right", markerscale=4)
    save(fig, out, "raan.png")


def plot_threshold(sats: list[Sat], shells: list[Shell], out: Path) -> None:
    fig, axes = panels(len(shells))
    for ax, shell in zip(axes, shells, strict=True):
        raan = np.array([s.raan for s in candidates(sats, shell)])
        stats = [plane_stats(raan, t, shell.min_plane_size) for t in THRESHOLDS_DEG]
        ax.plot(THRESHOLDS_DEG, [x["planes"] for x in stats], color=BLUE, label="planes")
        ax.plot(THRESHOLDS_DEG, [x["max"] for x in stats], color=ORANGE, label="largest plane")
        ax.plot(THRESHOLDS_DEG, [x["median"] for x in stats], color=AQUA, label="median plane")
        ax.axvline(shell.gap_deg, color=GRAY, linestyle="--", linewidth=1.0, label="proposed")
        ax.set_xscale("log")
        ax.set_xlabel("plane gap threshold (deg)")
        ax.set_title(f"{shell.name}: planes of {shell.min_plane_size} or more")
    axes[0].set_ylabel("count")
    axes[0].legend(loc="upper left")
    save(fig, out, "threshold.png")


def plot_plane_sizes(sats: list[Sat], shells: list[Shell], out: Path) -> None:
    fig, axes = panels(len(shells))
    for ax, shell in zip(axes, shells, strict=True):
        sizes: dict[int, int] = {}
        for s in sats:
            if s.shell == shell.name and s.plane is not None:
                sizes[s.plane] = sizes.get(s.plane, 0) + 1
        values = np.array(list(sizes.values()))
        ax.hist(values, bins=np.arange(0.5, values.max() + 1.5, 1.0), color=BLUE)
        ax.set_xlabel("satellites in the plane")
        ax.set_title(f"{shell.name}: {len(values)} planes")
    axes[0].set_ylabel("planes")
    save(fig, out, "plane_sizes.png")


def plot_spacing(sats: list[Sat], shells: list[Shell], out: Path) -> None:
    fig, axes = panels(len(shells))
    for ax, shell in zip(axes, shells, strict=True):
        gaps, _ = in_plane_gaps(sats, shell)
        ax.hist(gaps, bins=np.arange(0, 181, 1.0), color=BLUE)
        ax.set_yscale("log")
        ax.set_xlabel("gap to the next satellite along the orbit (deg)")
        ax.set_title(f"{shell.name}: median {np.median(gaps):.1f}, max {gaps.max():.1f}")
    axes[0].set_ylabel("gaps per degree (log)")
    save(fig, out, "spacing.png")


def drift_groups(drift: np.ndarray, gap: float) -> list[np.ndarray]:
    """Indices grouped by drift rate: sorted, cut where neighbours differ by more than gap."""
    order = np.argsort(drift, kind="stable")
    cuts = np.flatnonzero(np.diff(drift[order]) > gap) + 1
    return np.split(order, cuts)


def shell53_variants(
    sats: list[Sat], shell: Shell, raw_raan: dict[int, float]
) -> dict[str, list[dict]]:
    """Planes against threshold for the ways of choosing the 53 degree satellites tried."""
    lasers_in_inc = [
        s
        for s in sats
        if abs(s.inc - shell.inclination_deg) <= shell.tolerance_deg
        and s.reason not in ("no_lasers", "unknown_launch", "low_group")
    ]
    wide = [s for s in lasers_in_inc if 440.0 <= s.height < 500.0]
    narrow = candidates(sats, shell)
    variants: dict[str, list[dict]] = {}
    variants["raw element RAAN, proposed band"] = [
        plane_stats(np.array([raw_raan[s.norad_cat_id] for s in narrow]), t, shell.min_plane_size)
        for t in THRESHOLDS_DEG
    ]
    variants["common time, one band 440-500 km"] = [
        plane_stats(np.array([s.raan for s in wide]), t, shell.min_plane_size)
        for t in THRESHOLDS_DEG
    ]
    variants["common time, proposed band"] = [
        plane_stats(np.array([s.raan for s in narrow]), t, shell.min_plane_size)
        for t in THRESHOLDS_DEG
    ]
    # Split the wide band by RAAN drift rate (0.002 deg/day gaps), then cluster each group.
    drift = np.array([s.drift for s in wide])
    raan = np.array([s.raan for s in wide])
    groups = drift_groups(drift, 0.002)
    split = []
    for t in THRESHOLDS_DEG:
        per_group = [plane_stats(raan[g], t, shell.min_plane_size) for g in groups]
        split.append(
            {
                "planes": sum(x["planes"] for x in per_group),
                "in_planes": sum(x["in_planes"] for x in per_group),
                "median": float("nan"),
                "max": max(x["max"] for x in per_group),
            }
        )
    variants[f"common time, 440-500 km split by drift ({len(groups)} groups)"] = split
    return variants


def plot_shell53(sats: list[Sat], shell: Shell, variants: dict[str, list[dict]], out: Path) -> None:
    fig, (left, right) = plt.subplots(1, 2, figsize=(11, 4.0))
    near = [
        s
        for s in sats
        if abs(s.inc - shell.inclination_deg) <= shell.tolerance_deg
        and 440.0 <= s.height < 500.0
        and not np.isnan(s.drift)
    ]
    first_gen = [s for s in near if s.reason == "no_lasers"]
    rest = [s for s in near if s.reason != "no_lasers"]
    left.scatter(
        [s.height for s in rest],
        [s.drift for s in rest],
        s=3,
        color=BLUE,
        linewidths=0,
        label="with lasers",
    )
    left.scatter(
        [s.height for s in first_gen],
        [s.drift for s in first_gen],
        s=3,
        color=ORANGE,
        linewidths=0,
        label="first generation",
    )
    left.axvspan(shell.min_height_km, shell.max_height_km, color=BAND, label="proposed band")
    left.set_xlabel("mean geodetic height (km)")
    left.set_ylabel("RAAN drift (deg/day)")
    left.set_title(f"{shell.name} inclination band, 440-500 km")
    left.legend(loc="lower left", markerscale=4)
    colours = [GRAY, ORANGE, BLUE, AQUA]
    for (label, stats), colour in zip(variants.items(), colours, strict=True):
        right.plot(THRESHOLDS_DEG, [x["planes"] for x in stats], color=colour, label=label)
    right.axvline(shell.gap_deg, color=GRAY, linestyle="--", linewidth=1.0)
    right.set_xscale("log")
    right.set_xlabel("plane gap threshold (deg)")
    right.set_ylabel(f"planes of {shell.min_plane_size} or more")
    right.set_title(f"{shell.name}: how the satellites are chosen")
    right.legend(loc="upper left", fontsize=8)
    save(fig, out, "shell53.png")


def threshold_table(raan: np.ndarray, min_size: int) -> list[str]:
    lines = ["    threshold  planes  in planes  median  max"]
    for t in TABLE_THRESHOLDS_DEG:
        x = plane_stats(raan, t, min_size)
        counts = f"{x['planes']:6d}  {x['in_planes']:9d}  {x['median']:6.1f}  {x['max']:3d}"
        lines.append(f"    {t:9.2f}  {counts}")
    return lines


def angle_difference(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    return np.abs((a - b + 180.0) % 360.0 - 180.0)


def averaging_lines(sats: list[Sat], shells: list[Shell]) -> list[str]:
    """How much averaging over one orbit changes the answer, against osculating values."""
    lines = ["Averaging over one orbit against osculating values at the reference time"]
    for shell in shells:
        members = candidates(sats, shell)
        raan = np.array([s.raan for s in members])
        osc_raan = np.array([s.osc_raan for s in members])
        d_raan = angle_difference(raan, osc_raan)
        d_u = angle_difference(
            np.array([s.u for s in members]), np.array([s.osc_u for s in members])
        )
        d_inc = np.abs(np.array([s.inc for s in members]) - np.array([s.osc_inc for s in members]))
        mean_groups = {tuple(sorted(int(i) for i in c)) for c in clusters(raan, shell.gap_deg)}
        osc_groups = {tuple(sorted(int(i) for i in c)) for c in clusters(osc_raan, shell.gap_deg)}
        lines.append(
            f"  {shell.name}: |osculating - mean| RAAN median {np.median(d_raan):.3f} max "
            f"{d_raan.max():.3f} deg; argument of latitude median {np.median(d_u):.3f} max "
            f"{d_u.max():.3f} deg; inclination max {d_inc.max():.4f} deg; RAAN groups at "
            f"{shell.gap_deg} deg identical: {mean_groups == osc_groups} "
            f"({len(mean_groups ^ osc_groups)} groups differ)"
        )
    return lines


def check_against_program(sats: list[Sat], shells: list[Shell]) -> None:
    """The re-implemented clustering must give the program's planes at its thresholds."""
    for shell in shells:
        members = candidates(sats, shell)
        raan = np.array([s.raan for s in members])
        mine = {
            frozenset(members[int(i)].norad_cat_id for i in c)
            for c in clusters(raan, shell.gap_deg)
            if len(c) >= shell.min_plane_size
        }
        program: dict[int, set[int]] = {}
        for s in members:
            if s.plane is not None:
                program.setdefault(s.plane, set()).add(s.norad_cat_id)
        theirs = {frozenset(v) for v in program.values()}
        if mine != theirs:
            raise SystemExit(f"error: {shell.name}: re-implemented planes differ from the program")


def tables(
    sats: list[Sat], shells: list[Shell], variants: dict[str, list[dict]], summary: str
) -> str:
    out = io.StringIO()
    out.write(f"Shells and planes at {REFERENCE_TIME} UTC (data/starlink/2026-11-18)\n\n")
    out.write("Program summary\n")
    out.write(summary)
    out.write("\n")
    for shell in shells:
        members = candidates(sats, shell)
        raan = np.array([s.raan for s in members])
        gaps, largest = in_plane_gaps(sats, shell)
        sizes: dict[int, int] = {}
        for s in sats:
            if s.shell == shell.name and s.plane is not None:
                sizes[s.plane] = sizes.get(s.plane, 0) + 1
        size_values = np.array(list(sizes.values()))
        values, counts = np.unique(np.round(gaps, 1), return_counts=True)
        out.write(
            f"{shell.name}: inclination {shell.inclination_deg} +- {shell.tolerance_deg} deg, "
            f"height {shell.min_height_km} to {shell.max_height_km} km, plane gap "
            f"{shell.gap_deg} deg, planes of {shell.min_plane_size} or more\n"
        )
        out.write(
            f"  candidates {len(members)}, in planes {int(size_values.sum())}, planes "
            f"{len(size_values)}; plane size min {size_values.min()} median "
            f"{np.median(size_values):.1f} max {size_values.max()}; most common size "
            f"{np.bincount(size_values).argmax()} ({np.bincount(size_values).max()} planes)\n"
        )
        out.write(
            f"  gaps along the orbit: most common {values[counts.argmax()]:.1f} deg, median "
            f"{np.median(gaps):.1f}, p95 {np.percentile(gaps, 95):.1f}, max {gaps.max():.1f}; "
            f"largest per plane (deg, plane): "
            + ", ".join(f"{g:.1f} ({p})" for g, p in largest[:5])
            + "\n"
        )
        out.write("  plane count against the gap threshold:\n")
        out.write("\n".join(threshold_table(raan, shell.min_plane_size)) + "\n\n")
    out.write(
        "53 degree shell: planes of 10 or more against the threshold, by method "
        "(each column: planes / satellites in planes / largest plane)\n"
    )
    for k, name in enumerate(variants):
        out.write(f"  ({k + 1}) {name}\n")
    header = "    threshold  " + "  ".join(f"{f'({k + 1})':>18}" for k in range(len(variants)))
    out.write(header + "\n")
    for t in TABLE_THRESHOLDS_DEG:
        k = int(np.argmin(np.abs(THRESHOLDS_DEG - t)))
        row = "  ".join(
            f"{s[k]['planes']:>5d} / {s[k]['in_planes']:>4d} / {s[k]['max']:>4d}"
            for s in variants.values()
        )
        out.write(f"    {THRESHOLDS_DEG[k]:9.2f}  {row}\n")
    out.write("\n")
    out.write("\n".join(averaging_lines(sats, shells)) + "\n")
    return out.getvalue()


def main(argv: list[str]) -> int:
    st.forbid_network()
    parser = argparse.ArgumentParser(description="Regenerate the Phase 2 shell study.")
    parser.add_argument("--constellation", type=Path, required=True, help="built program")
    parser.add_argument("--snapshot", type=Path, default=st.DEFAULT_SNAPSHOT)
    parser.add_argument("--time", default=REFERENCE_TIME, help="reference time, UTC")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output directory")
    args = parser.parse_args(argv)
    st.check_snapshot(args.snapshot)
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="satsim-shells-") as work_dir:
        csv_path, summary = run_constellation(
            args.constellation, args.snapshot, args.time, Path(work_dir)
        )
        sats = read_csv(csv_path)
    shells = parse_shells(summary)
    check_against_program(sats, shells)
    raw_raan = {
        number: float(chosen.record["RA_OF_ASC_NODE"])
        for number, chosen in st.load_snapshot_elements(args.snapshot).items()
    }
    plot_heights(sats, shells, args.out)
    plot_inclinations(sats, shells, args.out)
    plot_raan(sats, shells, args.out)
    plot_threshold(sats, shells, args.out)
    plot_plane_sizes(sats, shells, args.out)
    plot_spacing(sats, shells, args.out)
    shell53 = next(s for s in shells if s.name.startswith("53"))
    variants = shell53_variants(sats, shell53, raw_raan)
    plot_shell53(sats, shell53, variants, args.out)
    text = tables(sats, shells, variants, summary)
    (args.out / "tables.txt").write_text(text, encoding="utf-8")
    print(f"wrote {args.out / 'tables.txt'}")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

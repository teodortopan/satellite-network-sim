#!/usr/bin/env python3
"""The Phase 2 report's plots, from the outputs of python/run_phase2.py.

Reads the runs' CSVs (results/phase-2/, not committed) and the committed summaries
(docs/phase-2-data/), and writes to docs/img/phase-2/:

  delay_over_time.png     one-way delay over the 12 hours for each study pair, realistic
                          against best case (pattern three).
  delay_by_pattern.png    best-case one-way delay distribution per pair and pattern.
  stretch.png             median and 95th-percentile stretch per pair and direction, both
                          models (pattern three).
  sensitivity.png         median one-way delay against each changed parameter, relative to
                          the default run.
  timestep.png            what the time step changes: path changes per hour and the largest
                          jump in delay at 1 s, 15 s and 60 s.
  high_latitude.png       Reykjavik to Vladivostok: path status and laser hops on the 53 deg
                          shell alone against all shells (best case and realistic).
  crosscheck.png          the ideal-shell cross-check: program against the independent model,
                          and Paris - Luanda against Hypatia's published values.

Usage (inside the CI image, after run_phase2.py and validate_topology.py --summary-out):

    python3 python/plot_phase2.py
"""

from __future__ import annotations

import argparse
import csv
import gzip
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import snapshot_tools as st

REPO = st.REPO_ROOT
DEFAULT_RESULTS = REPO / "results/phase-2"
DATA = REPO / "docs/phase-2-data"
DEFAULT_OUT = REPO / "docs/img/phase-2"
START = st.parse_utc("2026-11-18T21:15:00")

# The palette and style of the Phase 1 and constellation plots.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#dcdbd7"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
AQUA = "#1baf7a"
GRAY = "#7a7974"
PURPLE = "#8a5cd1"
PATTERN_COLOURS = {"three": BLUE, "plus-grid": ORANGE, "six": AQUA}
MODEL_COLOURS = {"realistic": ORANGE, "best-case": BLUE}

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
        "lines.linewidth": 1.4,
    }
)

STUDY_PAIRS = [
    ("New York", "London"),
    ("San Francisco", "Tokyo"),
    ("Frankfurt", "Singapore"),
    ("Detroit", "Buenos Aires"),
    ("New York", "Chicago"),
    ("Reykjavik", "Vladivostok"),
]


def save(fig, out: Path, name: str) -> None:
    fig.tight_layout()
    path = out / name
    # "tight" keeps the legends placed under the axes inside the image.
    fig.savefig(path, dpi=110, bbox_inches="tight", pad_inches=0.15)
    plt.close(fig)
    print(f"wrote {path}")


def legend_below(fig, ax=None, ncol: int = 3, y: float = -0.02) -> None:
    """One legend under the whole figure, so it never covers data."""
    handles, labels = (ax or fig.axes[0]).get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, y), ncol=ncol)


def read_run(results: Path, name: str) -> list[dict]:
    with gzip.open(results / f"{name}.csv.gz", "rt", newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def read_data(name: str) -> list[dict]:
    with (DATA / name).open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def hours(r: dict) -> float:
    return (st.parse_utc(r["utc"]).unix_ns - START.unix_ns) / 3.6e12


def series(rows: list[dict], a: str, b: str) -> tuple[np.ndarray, np.ndarray]:
    """Time (h) and one-way delay (ms, NaN where there is no path) of one direction."""
    rs = [r for r in rows if r["from"] == a and r["to"] == b]
    t = np.array([hours(r) for r in rs])
    d = np.array([float(r["one_way_ms"]) if r["status"] == "ok" else np.nan for r in rs])
    return t, d


# ---------------------------------------------------------------------------------------
def plot_delay_over_time(results: Path, out: Path) -> None:
    realistic = read_run(results, "main-three-realistic")
    best = read_run(results, "main-three-best-case")
    fig, axes = plt.subplots(3, 2, figsize=(11, 8.5), sharex=True)
    for ax, (a, b) in zip(axes.flat, STUDY_PAIRS, strict=True):
        for rows, model in ((realistic, "realistic"), (best, "best-case")):
            t, d = series(rows, a, b)
            ax.plot(t, d, color=MODEL_COLOURS[model], label=f"{model}, first city to second")
            t2, d2 = series(rows, b, a)
            ax.plot(
                t2,
                d2,
                color=MODEL_COLOURS[model],
                alpha=0.45,
                linewidth=0.9,
                label=f"{model}, reverse direction",
            )
        lower = float(next(r for r in best if (r["from"], r["to"]) == (a, b))["lower_bound_ms"])
        ax.axhline(lower, color=GRAY, linestyle="--", linewidth=1.0, label="lower bound")
        ax.set_title(f"{a} - {b}")
        ax.set_ylabel("one-way delay (ms)")
        ax.set_ylim(bottom=0)
    for ax in axes[-1]:
        ax.set_xlabel("hours from 2026-11-18 21:15 UTC")
    legend_below(fig, axes.flat[0], ncol=5)
    fig.suptitle(
        "One-way delay, pattern three. Gaps: no path (Vladivostok has no service in the "
        "realistic model, so Vladivostok to Reykjavik has no realistic line)."
    )
    save(fig, out, "delay_over_time.png")


def plot_delay_by_pattern(results: Path, out: Path) -> None:
    runs = {p: read_run(results, f"main-{p}-best-case") for p in ("three", "plus-grid", "six")}
    fig, ax = plt.subplots(figsize=(11, 4.6))
    width = 0.26
    for i, (a, b) in enumerate(STUDY_PAIRS):
        for j, (pattern, rows) in enumerate(runs.items()):
            _, d = series(rows, a, b)
            d = d[~np.isnan(d)]
            if d.size == 0:
                continue
            position = i + (j - 1) * width
            parts = ax.boxplot(
                d,
                positions=[position],
                widths=width * 0.85,
                whis=(0, 100),
                patch_artist=True,
                medianprops={"color": INK},
            )
            for box in parts["boxes"]:
                box.set_facecolor(PATTERN_COLOURS[pattern])
                box.set_alpha(0.8)
            if i == 0:
                ax.plot([], [], color=PATTERN_COLOURS[pattern], linewidth=6, label=pattern)
    ax.set_xticks(range(len(STUDY_PAIRS)))
    ax.set_xticklabels([f"{a}\nto {b}" for a, b in STUDY_PAIRS])
    ax.set_ylabel("one-way delay (ms)")
    ax.set_title(
        "Best case: delay over 12 h by laser pattern (box: quartiles; whiskers: min and max)"
    )
    ax.set_ylim(bottom=0)
    legend_below(fig, ax, ncol=3)
    save(fig, out, "delay_by_pattern.png")


def plot_stretch(out: Path) -> None:
    rows = [r for r in read_data("main.csv") if r["pattern"] == "three"]
    fig, ax = plt.subplots(figsize=(11, 4.6))
    labels = []
    directions = []
    for a, b in [*STUDY_PAIRS, ("Paris", "Luanda"), ("Oulu", "New York")]:
        directions += [(a, b), (b, a)]
    for i, (a, b) in enumerate(directions):
        labels.append(f"{a}\nto {b}")
        for k, model in enumerate(("realistic", "best-case")):
            r = next(x for x in rows if x["model"] == model and x["from"] == a and x["to"] == b)
            if not r["stretch_median"]:
                ax.text(
                    i + (k - 0.5) * 0.36,
                    0.15,
                    "no path",
                    rotation=90,
                    ha="center",
                    va="bottom",
                    fontsize=7,
                    color=INK_SECONDARY,
                )
                continue
            median, p95 = float(r["stretch_median"]), float(r["stretch_p95"])
            x = i + (k - 0.5) * 0.36
            ax.bar(
                x,
                median,
                width=0.34,
                color=MODEL_COLOURS[model],
                label=f"{model} median" if i == 0 else None,
            )
            ax.plot(
                [x, x],
                [median, p95],
                color=INK,
                linewidth=1.0,
                label="up to the 95th percentile" if i == 0 and k == 0 else None,
            )
    ax.axhline(3.0, color=GRAY, linestyle="--", linewidth=1.0, label="stretch 3 (flagged above)")
    ax.set_xticks(range(len(directions)))
    ax.set_xticklabels(labels, fontsize=7, rotation=90)
    ax.set_ylabel("stretch (delay / lower bound)")
    ax.set_title("Stretch per direction, pattern three")
    legend_below(fig, ax, ncol=4)
    save(fig, out, "stretch.png")


SENSITIVITY_PARAMETERS = [
    ("range-cap-km", "range cap (km)", ["1000", "2000", "3000", "none"], "none"),
    ("sun-exclusion-deg", "Sun exclusion (deg)", ["off", "5", "10"], "off"),
    ("lock-on-s", "lock-on time (s)", ["0", "15", "60"], "0"),
    ("fibre-factor", "fibre factor", ["1.5", "1.98", "2.5"], "1.98"),
    ("high-latitude-mask", "5 deg mask above 62 deg", ["off", "on"], "off"),
]


def sensitivity_row(sens, base_rows, model, name, value, default, a, b):
    if value == default:
        return next(
            (r for r in base_rows if r["model"] == model and r["from"] == a and r["to"] == b),
            None,
        )
    return next(
        (
            x
            for x in sens
            if x["model"] == model
            and x["parameter"] == f"{name}={value}"
            and x["from"] == a
            and x["to"] == b
        ),
        None,
    )


def plot_sensitivity(out: Path) -> None:
    """Rows: best case median change, best case steps without a path, realistic median change,
    realistic steps without a path. Medians are over the steps that have a path, so the share
    of steps without one is shown next to them."""
    sens = read_data("sensitivity.csv")
    main = [r for r in read_data("main.csv") if r["pattern"] == "three"]
    fig, axes = plt.subplots(4, 5, figsize=(13, 11), sharey="row")
    colours = [BLUE, ORANGE, AQUA, PURPLE, GRAY, INK]
    for block, model in enumerate(("best-case", "realistic")):
        for col, (name, label, values, default) in enumerate(SENSITIVITY_PARAMETERS):
            delay_ax, gap_ax = axes[2 * block][col], axes[2 * block + 1][col]
            if model == "best-case" and name == "fibre-factor":
                for ax in (delay_ax, gap_ax):
                    ax.text(
                        0.5,
                        0.5,
                        "no fibre in\nthe best case",
                        ha="center",
                        va="center",
                        transform=ax.transAxes,
                        color=INK_SECONDARY,
                    )
                    ax.set_xticks([])
                delay_ax.set_title(f"{model}: {label}", fontsize=8)
                continue
            for (a, b), colour in zip(STUDY_PAIRS, colours, strict=True):
                base = sensitivity_row(sens, main, model, name, default, default, a, b)
                if base is None or not base["one_way_median_ms"]:
                    continue
                change, missing = [], []
                for v in values:
                    r = sensitivity_row(sens, main, model, name, v, default, a, b)
                    has_path = r is not None and r["one_way_median_ms"]
                    change.append(
                        100.0
                        * (float(r["one_way_median_ms"]) / float(base["one_way_median_ms"]) - 1)
                        if has_path
                        else np.nan
                    )
                    missing.append(
                        100.0 * (1.0 - int(r["ok"]) / int(r["steps"])) if r is not None else np.nan
                    )
                delay_ax.plot(values, change, marker="o", color=colour, label=f"{a} to {b}")
                gap_ax.plot(values, missing, marker="o", color=colour)
            delay_ax.axhline(0.0, color=GRAY, linewidth=0.8)
            delay_ax.set_title(f"{model}: {label}", fontsize=8)
            if col == 0:
                delay_ax.set_ylabel(f"{model}:\nmedian delay vs default (%)")
                gap_ax.set_ylabel(f"{model}:\nsteps without a path (%)")
    legend_below(fig, axes[0][0], ncol=6)
    save(fig, out, "sensitivity.png")


def plot_timestep(out: Path) -> None:
    rows = [r for r in read_data("timestep.csv") if r["pattern"] == "three"]
    fig, axes = plt.subplots(2, 2, figsize=(11, 7.0))
    directions = [(a, b) for a, b in STUDY_PAIRS if a != "Reykjavik"] + [
        ("Reykjavik", "Vladivostok")
    ]
    x = np.arange(len(directions))
    for row, model in enumerate(("realistic", "best-case")):
        for col, (metric, label, per_hour) in enumerate(
            (
                ("path_changes", "path changes per hour", True),
                ("largest_jump_ms", "largest jump between consecutive steps (ms)", False),
            )
        ):
            ax = axes[row][col]
            for k, (series_name, hours_long, colour) in enumerate(
                (
                    ("1s-first-hour", 1.0, BLUE),
                    ("15s-first-hour", 1.0, ORANGE),
                    ("15s-12h", 12.0, AQUA),
                    ("60s-12h", 12.0, PURPLE),
                )
            ):
                ys = []
                for a, b in directions:
                    r = next(
                        (
                            z
                            for z in rows
                            if z["model"] == model
                            and z["from"] == a
                            and z["to"] == b
                            and z["series"] == series_name
                        ),
                        None,
                    )
                    value = float(r[metric]) if r and r[metric] else np.nan
                    ys.append(value / hours_long if per_hour else value)
                ax.bar(x + (k - 1.5) * 0.2, ys, width=0.19, color=colour, label=series_name)
            ax.set_xticks(x)
            ax.set_xticklabels([f"{a}\nto {b}" for a, b in directions], fontsize=7)
            ax.set_title(f"{model}: {label}", fontsize=9)
            if per_hour:
                ax.set_yscale("log")
    legend_below(fig, axes[0][0], ncol=4)
    save(fig, out, "timestep.png")


def plot_high_latitude(results: Path, out: Path) -> None:
    """Reykjavik to Vladivostok: which shells can carry it at all, and how the best case
    behaves on all shells against the two shells that reach 64 deg north alone."""
    configurations = [
        ("all four shells", "main-three"),
        ("53 deg alone", "dev-three"),
        ("43 deg alone", "shell-43deg-three"),
        ("97 deg alone", "shell-97deg-three"),
        ("70 deg alone", "shell-70deg-three"),
    ]
    statuses = [
        ("ok", "path", BLUE),
        ("no_gateway", "no gateway", ORANGE),
        ("no_path", "no path", PURPLE),
        ("no_coverage", "no satellite above the mask", GRAY),
        ("no_service", "no service (no home PoP)", INK),
    ]
    fig = plt.figure(figsize=(12, 7.5))
    grid = fig.add_gridspec(2, 2, width_ratios=[1, 1.5])
    bars = fig.add_subplot(grid[:, 0])
    labels, position = [], 0
    for model in ("best-case", "realistic"):
        for config_label, prefix in configurations:
            rows = read_run(results, f"{prefix}-{model}")
            rs = [r for r in rows if r["from"] == "Reykjavik" and r["to"] == "Vladivostok"]
            left = 0.0
            for status, status_label, colour in statuses:
                share = 100.0 * sum(r["status"] == status for r in rs) / len(rs)
                bars.barh(
                    position,
                    share,
                    left=left,
                    color=colour,
                    label=status_label if position == 0 else None,
                )
                left += share
            labels.append(f"{model}, {config_label}")
            position += 1
    bars.set_yticks(range(len(labels)))
    bars.set_yticklabels(labels, fontsize=8)
    bars.invert_yaxis()
    bars.set_xlabel("share of the 2,881 steps (%)")
    bars.set_title("Reykjavik to Vladivostok, pattern three: status by shells used")
    bars.legend(loc="upper center", bbox_to_anchor=(0.5, -0.08), ncol=2, fontsize=8)

    delay = fig.add_subplot(grid[0, 1])
    hops = fig.add_subplot(grid[1, 1], sharex=delay)
    for (config_label, prefix), colour in zip(
        [configurations[0], configurations[4], configurations[3]],
        (BLUE, AQUA, ORANGE),
        strict=True,
    ):
        rows = read_run(results, f"{prefix}-best-case")
        rs = [r for r in rows if r["from"] == "Reykjavik" and r["to"] == "Vladivostok"]
        t = np.array([hours(r) for r in rs])
        d = np.array([float(r["one_way_ms"]) if r["status"] == "ok" else np.nan for r in rs])
        h = np.array([float(r["laser_hops"]) if r["status"] == "ok" else np.nan for r in rs])
        delay.plot(t, d, color=colour, linewidth=0.9, label=config_label)
        hops.plot(t, h, color=colour, linewidth=0.8, label=config_label)
    delay.set_title("Best case: one-way delay (gaps: no path)")
    delay.set_ylabel("one-way delay (ms)")
    hops.set_ylabel("laser hops")
    hops.set_xlabel("hours from 2026-11-18 21:15 UTC")
    hops.legend(loc="upper center", bbox_to_anchor=(0.5, -0.15), ncol=3, fontsize=8)
    save(fig, out, "high_latitude.png")


def plot_crosscheck(out: Path) -> None:
    rows = read_data("crosscheck.csv")
    fig, (left, right) = plt.subplots(
        1, 2, figsize=(11, 4.4), gridspec_kw={"width_ratios": [1.6, 1]}
    )
    diffs = [r for r in rows if r["kind"] == "program_minus_model_one_way"]
    labels = [f"{r['from']} to {r['to']}" for r in diffs]
    values = [float(r["worst_abs_difference_ms"]) for r in diffs]
    left.barh(range(len(values)), values, color=BLUE, label="worst |program - model| over 61 times")
    left.axvline(1e-6, color=ORANGE, linestyle="--", label="tolerance, 1e-6 ms")
    left.set_xscale("log")
    left.set_xlim(1e-13, 1e-5)
    left.set_yticks(range(len(labels)))
    left.set_yticklabels(labels, fontsize=7)
    left.set_xlabel("one-way delay difference (ms)")
    left.set_title("Ideal shell, plus-grid, best case: program against the Python model")
    left.legend(loc="upper center", bbox_to_anchor=(0.5, -0.13), ncol=1, fontsize=8)

    ranges = [("published by Hypatia", 85.0, 117.0, GRAY)]
    for r in rows:
        if r["from"] != "Paris" or r["to"] != "Luanda":
            continue
        if r["kind"] == "program_round_trip_200s":
            ranges.append(
                ("program (nearest-slot +Grid)", float(r["min_ms"]), float(r["max_ms"]), BLUE)
            )
        elif "same-index" in r["kind"]:
            ranges.append(
                (
                    "model, Hypatia's grid, 25 deg mask",
                    float(r["min_ms"]),
                    float(r["max_ms"]),
                    ORANGE,
                )
            )
        elif "ground-link rule" in r["kind"]:
            ranges.append(
                (
                    "model, Hypatia's grid and ground rule",
                    float(r["min_ms"]),
                    float(r["max_ms"]),
                    AQUA,
                )
            )
    for i, (_label, low, high, colour) in enumerate(ranges):
        right.plot([low, high], [i, i], color=colour, linewidth=6, solid_capstyle="butt")
        right.text(high + 2, i, f"{low:.1f} to {high:.1f} ms", va="center", fontsize=8)
    right.set_yticks(range(len(ranges)))
    right.set_yticklabels([x[0] for x in ranges], fontsize=8)
    right.set_xlim(0, 150)
    right.set_xlabel("round trip over 200 s (ms)")
    right.set_title("Paris - Luanda on Hypatia's shell")
    save(fig, out, "crosscheck.png")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Draw the Phase 2 report's plots.")
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)
    if not (args.results / "main-three-realistic.csv.gz").is_file():
        print(
            f"error: no run outputs in {args.results} (run python/run_phase2.py)", file=sys.stderr
        )
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    plot_delay_over_time(args.results, args.out)
    plot_delay_by_pattern(args.results, args.out)
    plot_stretch(args.out)
    plot_sensitivity(args.out)
    plot_timestep(args.out)
    plot_high_latitude(args.results, args.out)
    plot_crosscheck(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

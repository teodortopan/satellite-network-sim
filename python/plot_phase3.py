#!/usr/bin/env python3
"""The Phase 3 report's plots, from the outputs of python/run_phase3.py and the validation
scripts.

Reads the committed summaries (docs/phase-3-data/: measures.csv, drop_offsets.csv,
access_elevation.csv, bariloche_beijing.csv, hypatia_path_changes.csv, lens_victoria.csv) and,
for route_over_time.png and route_lifetime.png, the routes CSVs of the runs (results/phase-3/,
not committed), and writes to docs/img/phase-3/:

  strategies_best_case.png  run main-best-case: every strategy, per study pair: path changes
                            per hour (bad and increases), breaks per hour, share of time with
                            no route, median extra delay over the ideal.
  strategies_realistic.png  the same for main-realistic.
  route_over_time.png       New York to London, the first 10 minutes: one-way delay of the
                            ideal, lowest-delay, keep-until-broken and longest-lived, with the
                            dropped steps and the 15 s re-plan instants; both models.
  replan_interval.png       share of time with no route and path changes per hour against the
                            re-plan interval (1, 15, 60 s), lowest-delay and keep-until-broken,
                            both models, with the whole-interval planner at 15 s.
  drop_offsets.png          drop events by the second since the last re-plan at which they
                            start.
  access_elevation.png      elevation of the access satellites at replaced plans, lowest-delay
                            against longest-lived, against the 20 deg mask.
  serving_rules.png         realistic: the four serving-satellite rules, lowest-delay and
                            longest-lived.
  patterns.png              laser patterns three, plus-grid and six, lowest-delay, both models.
  route_lifetime.png        cumulative distribution of route lifetime, five strategies, New
                            York to London and Reykjavik to Vladivostok, both models.
  threshold.png             the threshold strategy's trade-off: path changes per hour against
                            median extra delay, best case.
  validation.png            Bariloche to Beijing and Hypatia's path changes against the
                            published numbers, and LENS Victoria's measured round trips
                            against the model.

Only the forward direction of each pair (the first city to the second) is shown. In the best
case the two directions of a pair are identical; in the realistic model Vladivostok has no
home PoP, so Vladivostok to Reykjavik has no service throughout and is left out.

Usage (inside the CI image, after run_phase3.py, reproduce_routing.py and compare_lens.py):

    python3 python/plot_phase3.py
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

import run_phase3 as rp
import snapshot_tools as st

REPO = st.REPO_ROOT
DEFAULT_RESULTS = REPO / "results/phase-3"
DATA = REPO / "docs/phase-3-data"
DEFAULT_OUT = REPO / "docs/img/phase-3"
START = st.parse_utc(rp.START)
RUNS_NEEDED = ("main-best-case", "main-realistic")  # Routes CSVs read by plots 3 and 9.

# The palette and style of the Phase 1 and Phase 2 plots.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#dcdbd7"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
AQUA = "#1baf7a"
GRAY = "#7a7974"
PURPLE = "#8a5cd1"
MODEL_COLOURS = {"realistic": ORANGE, "best-case": BLUE}
PATTERN_COLOURS = {"three": BLUE, "plus-grid": ORANGE, "six": AQUA}
STRATEGY_COLOURS = {
    "lowest-delay": BLUE,
    "keep-until-broken": ORANGE,
    "longest-lived": AQUA,
    "threshold:1ms": PURPLE,
    "threshold:5%": GRAY,
}

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
STRATEGY_LABELS = {
    "lowest-delay": "lowest-delay",
    "keep-until-broken": "keep-until-broken",
    "longest-lived": "longest-lived",
    "threshold:0.5ms": "threshold 0.5 ms",
    "threshold:1ms": "threshold 1 ms",
    "threshold:2ms": "threshold 2 ms",
    "threshold:5ms": "threshold 5 ms",
    "threshold:1%": "threshold 1%",
    "threshold:2%": "threshold 2%",
    "threshold:5%": "threshold 5%",
    "threshold:10%": "threshold 10%",
}
MS_THRESHOLDS = ("threshold:0.5ms", "threshold:1ms", "threshold:2ms", "threshold:5ms")
PCT_THRESHOLDS = ("threshold:1%", "threshold:2%", "threshold:5%", "threshold:10%")
FORWARD_NOTE = "forward direction only (the first city to the second)"


def save(fig, out: Path, name: str, tight_layout: bool = True) -> None:
    if tight_layout:
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


def read_data(name: str) -> list[dict]:
    with (DATA / name).open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def pair_label(a: str, b: str) -> str:
    return f"{a} to {b}"


class Measures:
    """measures.csv, looked up by run, direction and strategy."""

    def __init__(self) -> None:
        self.rows = {
            (r["run"], r["from"], r["to"], r["strategy"]): r for r in read_data("measures.csv")
        }

    def row(self, run: str, a: str, b: str, strategy: str) -> dict:
        return self.rows[(run, a, b, strategy)]

    def value(self, run: str, a: str, b: str, strategy: str, column: str) -> float:
        text = self.row(run, a, b, strategy)[column]
        return float(text) if text else np.nan

    def no_route_pct(self, run: str, a: str, b: str, strategy: str) -> float:
        r = self.row(run, a, b, strategy)
        return 100.0 * int(r["no_route_s"]) / int(r["steps"])


def pair_titles(axes_row) -> None:
    for ax, (a, b) in zip(axes_row, STUDY_PAIRS, strict=True):
        ax.set_title(pair_label(a, b))


# ---------------------------------------------------------------------------------------
def plot_strategies(m: Measures, out: Path, run: str, name: str, title: str) -> None:
    strategies = rp.ALL_STRATEGIES
    x = np.arange(len(strategies))
    fig, axes = plt.subplots(4, 6, figsize=(16, 11), sharex=True, sharey="row")
    for col, (a, b) in enumerate(STUDY_PAIRS):
        bad = [m.value(run, a, b, s, "bad_per_h") for s in strategies]
        inc = [m.value(run, a, b, s, "increases_per_h") for s in strategies]
        axes[0][col].bar(x, bad, color=ORANGE, label="bad changes (delay goes down)")
        axes[0][col].bar(x, inc, bottom=bad, color=BLUE, label="delay increases")
        axes[1][col].bar(
            x,
            [m.value(run, a, b, s, "breaks_per_h") for s in strategies],
            color=PURPLE,
            label="breaks (a route, then a drop)",
        )
        axes[2][col].bar(
            x,
            [m.no_route_pct(run, a, b, s) for s in strategies],
            color=GRAY,
            label="time with no route",
        )
        axes[3][col].bar(
            x,
            [m.value(run, a, b, s, "extra_median_ms") for s in strategies],
            color=AQUA,
            label="median extra delay over the ideal",
        )
    pair_titles(axes[0])
    for ax, label in zip(
        axes[:, 0],
        (
            "path changes per hour",
            "breaks per hour",
            "time with no route (%)",
            "median extra delay\nover the ideal (ms)",
        ),
        strict=True,
    ):
        ax.set_ylabel(label)
    for ax in axes[-1]:
        ax.set_xticks(x)
        ax.set_xticklabels([STRATEGY_LABELS[s] for s in strategies], rotation=90, fontsize=8)
    handles, labels = [], []
    for ax in axes[:, 0]:
        h, lab = ax.get_legend_handles_labels()
        handles += h
        labels += lab
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, -0.01), ncol=5)
    fig.suptitle(f"{title}: every strategy, re-plan every 15 s, 2 h at 1 s; {FORWARD_NOTE}")
    save(fig, out, name)


# ---------------------------------------------------------------------------------------
ROUTE_STRATEGIES = ("lowest-delay", "keep-until-broken", "longest-lived")
FIRST_SECONDS = 600


def read_first_minutes(results: Path, run: str, a: str, b: str) -> dict:
    """Seconds from the start, one-way delay (NaN when dropped) and dropped flag per strategy,
    and the ideal, for the first FIRST_SECONDS of one direction. The routes CSV is in time
    order, so reading stops at the first row past the window."""
    t_of: dict[str, float] = {}
    lanes = {s: ([], [], []) for s in ROUTE_STRATEGIES}
    ideal_t, ideal = [], []
    with gzip.open(results / f"{run}.csv.gz", "rt", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            utc = r["utc"]
            if utc not in t_of:
                t_of[utc] = (st.parse_utc(utc).unix_ns - START.unix_ns) / st.NS_PER_S
            t = t_of[utc]
            if t >= FIRST_SECONDS:
                break
            if r["from"] != a or r["to"] != b or r["strategy"] not in lanes:
                continue
            ts, ds, dropped = lanes[r["strategy"]]
            ok = r["status"] == "ok"
            ts.append(t)
            ds.append(float(r["one_way_ms"]) if ok else np.nan)
            dropped.append(not ok)
            if r["strategy"] == "lowest-delay":
                ideal_t.append(t)
                ideal.append(float(r["ideal_ms"]) if r["ideal_ms"] else np.nan)
    return {
        "lanes": {s: tuple(np.array(v) for v in lane) for s, lane in lanes.items()},
        "ideal": (np.array(ideal_t), np.array(ideal)),
    }


def runs_of(flags: np.ndarray) -> list[tuple[int, int]]:
    """(start index, length) of every run of True."""
    spans, start = [], None
    for i, flag in enumerate(flags):
        if flag and start is None:
            start = i
        elif not flag and start is not None:
            spans.append((start, i - start))
            start = None
    if start is not None:
        spans.append((start, len(flags) - start))
    return spans


def plot_route_over_time(results: Path, out: Path) -> None:
    a, b = "New York", "London"
    fig = plt.figure(figsize=(12, 8.5))
    grid = fig.add_gridspec(4, 1, height_ratios=[3, 0.7, 3, 0.7], hspace=0.25, top=0.92, bottom=0.1)
    styles = {
        "lowest-delay": {"linewidth": 1.8},
        "keep-until-broken": {"linewidth": 1.4, "linestyle": (0, (4, 2))},
        "longest-lived": {"linewidth": 1.4},
    }
    first_delay_ax = None
    for block, (run, model) in enumerate(
        (("main-best-case", "best case"), ("main-realistic", "realistic"))
    ):
        data = read_first_minutes(results, run, a, b)
        delay = fig.add_subplot(grid[2 * block], sharex=first_delay_ax)
        strip = fig.add_subplot(grid[2 * block + 1], sharex=delay)
        first_delay_ax = first_delay_ax or delay
        for ax in (delay, strip):
            for k in range(0, FIRST_SECONDS + 1, rp.SLOT_S):
                ax.axvline(k, color=GRID, linewidth=0.7, zorder=0)
        t, ideal = data["ideal"]
        delay.plot(
            t, ideal, color=GRAY, linewidth=1.0, linestyle="--", label="ideal (lowest at each step)"
        )
        for i, s in enumerate(ROUTE_STRATEGIES):
            ts, ds, dropped = data["lanes"][s]
            delay.plot(ts, ds, color=STRATEGY_COLOURS[s], label=s, **styles[s])
            spans = [(ts[j], n * rp.STEP_S) for j, n in runs_of(dropped)]
            strip.broken_barh(spans, (i - 0.4, 0.8), color=STRATEGY_COLOURS[s])
        delay.set_ylabel("one-way delay (ms)")
        delay.set_title(
            f"{model}: {a} to {b}, first 10 minutes from 21:15 UTC (gaps: no route; "
            "light lines: re-plan every 15 s)"
        )
        delay.tick_params(labelbottom=False)
        strip.set_yticks(range(len(ROUTE_STRATEGIES)))
        strip.set_yticklabels([f"{s} dropped" for s in ROUTE_STRATEGIES], fontsize=7)
        strip.set_ylim(len(ROUTE_STRATEGIES) - 0.4, -0.6)
        strip.grid(False, axis="y")
        if block == 0:
            strip.tick_params(labelbottom=False)
        else:
            strip.set_xlabel("seconds from 2026-11-18 21:15:00 UTC")
    first_delay_ax.set_xlim(0, FIRST_SECONDS)
    legend_below(fig, first_delay_ax, ncol=4)
    fig.suptitle(
        "Route over time. Realistic: lowest-delay and keep-until-broken hold the same route "
        "here, so their lines lie on each other."
    )
    # The grid sets its own spacing; tight_layout cannot place a shared-axis grid like it.
    save(fig, out, "route_over_time.png", tight_layout=False)


# ---------------------------------------------------------------------------------------
def plot_replan_interval(m: Measures, out: Path) -> None:
    intervals = (1, 15, 60)
    fig, axes = plt.subplots(4, 6, figsize=(16, 11), sharex=True)
    for block, model in enumerate(("best-case", "realistic")):
        runs = (f"interval-1s-{model}", f"main-{model}", f"interval-60s-{model}")
        whole = f"planner-whole-interval-{model}"
        for col, (a, b) in enumerate(STUDY_PAIRS):
            gap_ax, change_ax = axes[2 * block][col], axes[2 * block + 1][col]
            for s in rp.TWO_STRATEGIES:
                gap_ax.plot(
                    intervals,
                    [m.no_route_pct(r, a, b, s) for r in runs],
                    marker="o",
                    color=STRATEGY_COLOURS[s],
                    label=f"{s}, planner at the re-plan instant",
                )
                change_ax.plot(
                    intervals,
                    [m.value(r, a, b, s, "changes_per_h") for r in runs],
                    marker="o",
                    color=STRATEGY_COLOURS[s],
                )
            gap_ax.plot(
                [15],
                [m.no_route_pct(whole, a, b, "lowest-delay")],
                marker="*",
                markersize=11,
                linestyle="none",
                color=INK,
                label="lowest-delay, planner over the whole interval (15 s)",
            )
            change_ax.plot(
                [15],
                [m.value(whole, a, b, "lowest-delay", "changes_per_h")],
                marker="*",
                markersize=11,
                linestyle="none",
                color=INK,
            )
            gap_ax.set_ylim(bottom=0)
            change_ax.set_ylim(bottom=0)
            gap_ax.set_title(f"{model}: {pair_label(a, b)}", fontsize=8)
            if col == 0:
                gap_ax.set_ylabel(f"{model}:\ntime with no route (%)")
                change_ax.set_ylabel(f"{model}:\npath changes per hour")
    for ax in axes[-1]:
        ax.set_xscale("log")
        ax.set_xticks(intervals)
        ax.set_xticklabels([str(i) for i in intervals])
        ax.minorticks_off()
        ax.set_xlabel("re-plan interval (s)")
    legend_below(fig, axes[0][0], ncol=3)
    fig.suptitle(f"What the re-plan interval changes; {FORWARD_NOTE}")
    save(fig, out, "replan_interval.png")


# ---------------------------------------------------------------------------------------
def plot_drop_offsets(out: Path) -> None:
    rows = read_data("drop_offsets.csv")

    def counts(run, strategy, cause):
        rs = [
            r for r in rows if r["run"] == run and r["strategy"] == strategy and r["cause"] == cause
        ]
        rs.sort(key=lambda r: int(r["offset_s"]))
        return np.array([int(r["offset_s"]) for r in rs]), np.array([int(r["events"]) for r in rs])

    panels = [
        (
            "main-best-case",
            "best case, re-plan every 15 s",
            ("lowest-delay", "longest-lived"),
            ("ground_link_lost",),
        ),
        (
            "main-realistic",
            "realistic, re-plan every 15 s",
            ("lowest-delay", "longest-lived"),
            ("ground_link_lost", "serving_left_view"),
        ),
        (
            "interval-60s-best-case",
            "best case, re-plan every 60 s",
            ("lowest-delay",),
            ("ground_link_lost",),
        ),
        (
            "interval-60s-realistic",
            "realistic, re-plan every 60 s",
            ("lowest-delay",),
            ("ground_link_lost", "serving_left_view"),
        ),
    ]
    colours = {
        ("lowest-delay", "ground_link_lost"): BLUE,
        ("longest-lived", "ground_link_lost"): AQUA,
        ("lowest-delay", "serving_left_view"): ORANGE,
        ("longest-lived", "serving_left_view"): PURPLE,
    }
    fig, axes = plt.subplots(2, 2, figsize=(13, 7.5))
    for ax, (run, title, strategies, causes) in zip(axes.flat, panels, strict=True):
        series = [(s, c) for s in strategies for c in causes]
        width = 0.8 / len(series)
        for k, (s, c) in enumerate(series):
            offsets, events = counts(run, s, c)
            ax.bar(
                offsets + (k - (len(series) - 1) / 2) * width,
                events,
                width=width,
                color=colours[(s, c)],
                label=f"{s}, {c} ({events.sum()} events)",
            )
        ax.set_title(f"{title} ({run})", fontsize=9)
        ax.set_xlabel("second since the last re-plan at which the drop starts")
        ax.set_ylabel("drop events (all 12 directions)")
        ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.2), ncol=2, fontsize=8)
    fig.suptitle(
        "When drops start inside the re-plan interval (second 0 is the re-plan instant; "
        "realistic serving slots start every 15 s)"
    )
    save(fig, out, "drop_offsets.png")


# ---------------------------------------------------------------------------------------
def plot_access_elevation(out: Path) -> None:
    rows = read_data("access_elevation.csv")

    def row(run, a, b, s, end):
        return next(
            r
            for r in rows
            if r["run"] == run
            and r["from"] == a
            and r["to"] == b
            and r["strategy"] == s
            and r["end"] == end
        )

    series_by_model = {
        "main-best-case": [
            ("lowest-delay", "first", BLUE, "o", "lowest-delay, satellite of the first city"),
            ("lowest-delay", "last", BLUE, "s", "lowest-delay, satellite of the second city"),
            ("longest-lived", "first", AQUA, "o", "longest-lived, satellite of the first city"),
            ("longest-lived", "last", AQUA, "s", "longest-lived, satellite of the second city"),
        ],
        "main-realistic": [
            (
                "lowest-delay",
                "first",
                BLUE,
                "o",
                "lowest-delay, serving satellite of the first city",
            ),
            (
                "longest-lived",
                "first",
                AQUA,
                "o",
                "longest-lived, serving satellite of the first city",
            ),
        ],
    }
    titles = {
        "main-best-case": "Best case: access satellites at replaced plans",
        "main-realistic": "Realistic: serving satellite at replaced plans\n"
        "(the serving rule picks it, not the strategy)",
    }
    fig, axes = plt.subplots(1, 2, figsize=(13, 5.2), sharey=True)
    for ax, (run, series) in zip(axes, series_by_model.items(), strict=True):
        span = 0.7
        for k, (s, end, colour, marker, label) in enumerate(series):
            dx = (k - (len(series) - 1) / 2) * span / len(series)
            for i, (a, b) in enumerate(STUDY_PAIRS):
                r = row(run, a, b, s, end)
                med, p5, p95 = (
                    float(r[c])
                    for c in ("elevation_median_deg", "elevation_p5_deg", "elevation_p95_deg")
                )
                ax.errorbar(
                    i + dx,
                    med,
                    yerr=[[med - p5], [p95 - med]],
                    color=colour,
                    marker=marker,
                    markerfacecolor=colour if end == "first" else SURFACE,
                    capsize=2,
                    linewidth=1.0,
                    label=label if i == 0 else None,
                )
                ax.text(
                    i + dx,
                    p95 + 1.0,
                    r["plans_replaced"],
                    ha="center",
                    va="bottom",
                    fontsize=6,
                    color=INK_SECONDARY,
                    rotation=90,
                )
        ax.axhline(
            20.0,
            color=ORANGE,
            linestyle="--",
            linewidth=1.0,
            label="20 deg mask (FCC, shells 400 to 500 km)",
        )
        ax.set_xticks(range(len(STUDY_PAIRS)))
        ax.set_xticklabels([f"{a}\nto {b}" for a, b in STUDY_PAIRS], fontsize=8)
        ax.set_title(titles[run], fontsize=9)
        ax.set_ylim(0, 100)
        ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.14), ncol=2, fontsize=8)
    axes[0].set_ylabel("elevation (deg): median, bar from 5th to 95th percentile")
    fig.suptitle(
        f"Elevation of the access satellite when a plan is replaced (small numbers: plans "
        f"replaced); {FORWARD_NOTE}"
    )
    save(fig, out, "access_elevation.png")


# ---------------------------------------------------------------------------------------
SERVING_RULES = [
    ("main-realistic", "highest\nelevation"),
    ("serving-longest-in-view", "longest\nin view"),
    ("serving-highest-whole-slot", "highest for\nwhole slot"),
    ("serving-with-route", "with\nroute"),
]


def plot_serving_rules(m: Measures, out: Path) -> None:
    strategies = ("lowest-delay", "longest-lived")
    x = np.arange(len(SERVING_RULES))
    width = 0.38
    fig, axes = plt.subplots(4, 6, figsize=(16, 11), sharex=True, sharey="row")
    for col, (a, b) in enumerate(STUDY_PAIRS):
        for k, s in enumerate(strategies):
            dx = (k - 0.5) * width
            runs = [run for run, _ in SERVING_RULES]
            serving = []
            for run in runs:
                r = m.row(run, a, b, s)
                serving.append(100.0 * int(r["serving_changes"]) / int(r["slot_starts"]))
            values = (
                [m.value(run, a, b, s, "changes_per_h") for run in runs],
                serving,
                [m.no_route_pct(run, a, b, s) for run in runs],
                [m.value(run, a, b, s, "extra_median_ms") for run in runs],
            )
            for ax, ys in zip(axes[:, col], values, strict=True):
                ax.bar(x + dx, ys, width=width * 0.95, color=STRATEGY_COLOURS[s], label=s)
    pair_titles(axes[0])
    for ax, label in zip(
        axes[:, 0],
        (
            "path changes per hour",
            "serving-satellite changes\n(% of slot starts)",
            "time with no route (%)",
            "median extra delay\nover the ideal (ms)",
        ),
        strict=True,
    ):
        ax.set_ylabel(label)
    for ax in axes[-1]:
        ax.set_xticks(x)
        ax.set_xticklabels([label for _, label in SERVING_RULES], fontsize=7)
        ax.set_xlabel("serving-satellite rule")
    legend_below(fig, axes[0][0], ncol=2)
    fig.suptitle(f"Realistic: the four serving-satellite rules, re-plan every 15 s; {FORWARD_NOTE}")
    save(fig, out, "serving_rules.png")


# ---------------------------------------------------------------------------------------
def plot_patterns(m: Measures, out: Path) -> None:
    patterns = [
        ("three", "main-{m}"),
        ("plus-grid", "pattern-plus-grid-{m}"),
        ("six", "pattern-six-{m}"),
    ]
    models = ("best-case", "realistic")
    x = np.arange(len(patterns))
    width = 0.38
    fig, axes = plt.subplots(3, 6, figsize=(16, 8.5), sharex=True, sharey="row")
    for col, (a, b) in enumerate(STUDY_PAIRS):
        for k, model in enumerate(models):
            dx = (k - 0.5) * width
            runs = [template.format(m=model) for _, template in patterns]
            values = (
                [m.value(run, a, b, "lowest-delay", "delay_median_ms") for run in runs],
                [m.no_route_pct(run, a, b, "lowest-delay") for run in runs],
                [m.value(run, a, b, "lowest-delay", "changes_per_h") for run in runs],
            )
            for ax, ys in zip(axes[:, col], values, strict=True):
                ax.bar(x + dx, ys, width=width * 0.95, color=MODEL_COLOURS[model], label=model)
    pair_titles(axes[0])
    for ax, label in zip(
        axes[:, 0],
        ("median one-way delay (ms)", "time with no route (%)", "path changes per hour"),
        strict=True,
    ):
        ax.set_ylabel(label)
    for ax in axes[-1]:
        ax.set_xticks(x)
        ax.set_xticklabels([p for p, _ in patterns])
        ax.set_xlabel("laser pattern")
    legend_below(fig, axes[0][0], ncol=2)
    fig.suptitle(f"Laser patterns, lowest-delay, re-plan every 15 s; {FORWARD_NOTE}")
    save(fig, out, "patterns.png")


# ---------------------------------------------------------------------------------------
LIFETIME_STRATEGIES = (
    "lowest-delay",
    "keep-until-broken",
    "longest-lived",
    "threshold:1ms",
    "threshold:5%",
)
LIFETIME_PAIRS = (("New York", "London"), ("Reykjavik", "Vladivostok"))


def plot_route_lifetime(results: Path, out: Path) -> None:
    runs = {run.name: run for run in rp.all_runs()}
    fig, axes = plt.subplots(2, 2, figsize=(11, 8), sharex=True, sharey=True)
    for col, (name, model) in enumerate(
        (("main-best-case", "best case"), ("main-realistic", "realistic"))
    ):
        _, lanes, _ = rp.read_lanes(results, runs[name])
        for row, (a, b) in enumerate(LIFETIME_PAIRS):
            ax = axes[row][col]
            for i, s in enumerate(LIFETIME_STRATEGIES):
                lifetimes = np.sort(np.array(lanes[(a, b, s)].lifetimes_s, dtype=float))
                share = np.arange(1, lifetimes.size + 1) / lifetimes.size
                ax.step(
                    lifetimes,
                    share,
                    where="post",
                    color=STRATEGY_COLOURS[s],
                    # Earlier lines wider, so lines that coincide still show as bands.
                    linewidth=3.4 - 0.6 * i,
                    label=f"{STRATEGY_LABELS[s]}",
                )
            ax.set_xscale("log")
            ax.set_ylim(0, 1.02)
            ax.set_title(f"{model}: {pair_label(a, b)}")
            if col == 0:
                ax.set_ylabel("share of routes living at most this long")
            if row == 1:
                ax.set_xlabel("route lifetime (s, log scale)")
    legend_below(fig, axes[0][0], ncol=5)
    fig.suptitle(
        "Route lifetime: consecutive steps with a route and the same route (runs cut by the "
        "start or end of the 2 h counted).\nForward direction; lines that coincide show as "
        "nested bands (realistic New York to London: all five)"
    )
    save(fig, out, "route_lifetime.png")


# ---------------------------------------------------------------------------------------
def threshold_point(m: Measures, run: str, a: str, b: str, s: str) -> tuple[float, float]:
    return (m.value(run, a, b, s, "changes_per_h"), m.value(run, a, b, s, "extra_median_ms"))


def plot_threshold(m: Measures, out: Path) -> None:
    """Threshold points carry their value, the ms series above right of the point and the %
    series below left of it; points of one series at one place share a label."""
    run = "main-best-case"
    fig, axes = plt.subplots(2, 3, figsize=(14, 8.5))
    for ax, (a, b) in zip(axes.flat, STUDY_PAIRS, strict=True):
        for strategies, colour, label, offset, (ha, va) in (
            (MS_THRESHOLDS, PURPLE, "threshold in ms (labels: ms)", (4, 4), ("left", "bottom")),
            (PCT_THRESHOLDS, GRAY, "threshold in % (labels: %)", (-4, -4), ("right", "top")),
        ):
            xy = [threshold_point(m, run, a, b, s) for s in strategies]
            ax.plot([p[0] for p in xy], [p[1] for p in xy], marker="o", color=colour, label=label)
            names: dict[tuple[float, float], list[str]] = {}
            for s, p in zip(strategies, xy, strict=True):
                names.setdefault(p, []).append(s.removeprefix("threshold:").removesuffix("ms"))
            for (px, py), values in names.items():
                ax.annotate(
                    ", ".join(values),
                    (px, py),
                    xytext=offset,
                    textcoords="offset points",
                    ha=ha,
                    va=va,
                    fontsize=7,
                    color=colour,
                )
        for s, colour, marker, dy in (
            ("lowest-delay", BLUE, "D", 20),
            ("keep-until-broken", ORANGE, "s", 18),
        ):
            px, py = threshold_point(m, run, a, b, s)
            ax.plot(
                [px], [py], marker=marker, markersize=8, linestyle="none", color=colour, label=s
            )
            ax.annotate(
                s,
                (px, py),
                xytext=(0, dy),
                textcoords="offset points",
                ha="center" if s == "lowest-delay" else "left",
                va="bottom",
                fontsize=7,
                color=colour,
            )
        ax.set_title(pair_label(a, b))
        ax.set_xlabel("path changes per hour")
        ax.set_ylabel("median extra delay over the ideal (ms)")
        ax.set_xlim(left=0, right=ax.get_xlim()[1] * 1.08)
        ax.margins(y=0.2)
    legend_below(fig, axes[0][0], ncol=4)
    fig.suptitle(
        "Best case: the threshold strategy's trade-off between path changes and delay; "
        f"{FORWARD_NOTE}"
    )
    save(fig, out, "threshold.png")


# ---------------------------------------------------------------------------------------
def plot_validation(out: Path) -> None:
    fig, (bar_ax, hyp_ax, lens_ax) = plt.subplots(
        1, 3, figsize=(16, 5.2), gridspec_kw={"width_ratios": [1.1, 1.2, 1]}
    )

    # Bariloche to Beijing (Stock et al., one orbit at 1 s, re-planned every step).
    rows = read_data("bariloche_beijing.csv")
    x = np.arange(len(rows))
    width = 0.2
    for k, (column, published, label, colour) in enumerate(
        (
            ("changes", False, "path changes, ours", BLUE),
            ("published_changes", True, "path changes, published", GRAY),
            ("bad", False, "bad changes, ours", ORANGE),
            ("published_bad", True, "bad changes, published", PURPLE),
        )
    ):
        ys = np.array([float(r[column]) for r in rows])
        xs = x + (k - 1.5) * width
        if published:
            bar_ax.bar(xs, ys, width=width * 0.95, color=colour, label=label)
            continue
        low_col, high_col = f"{column}_min_over_starts", f"{column}_max_over_starts"
        low = np.array([float(r[low_col]) for r in rows])
        high = np.array([float(r[high_col]) for r in rows])
        bar_ax.bar(
            xs,
            ys,
            width=width * 0.95,
            color=colour,
            label=label,
            yerr=[ys - low, high - ys],
            capsize=2,
            error_kw={"elinewidth": 0.9, "ecolor": INK},
        )
    bar_ax.plot([], [], color=INK, linewidth=0.9, label="ours: min to max over six start times")
    bar_ax.set_xticks(x)
    bar_ax.set_xticklabels([r["strategy"] for r in rows])
    bar_ax.set_ylabel("count over one orbit (5,739 s)")
    bar_ax.set_title("Bariloche to Beijing (Stock et al.): ours against published", fontsize=9)
    bar_ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.1), ncol=2, fontsize=8)

    # Hypatia (Kassing et al., Figure 8a): path changes per pair over 200 s at 100 ms.
    rows = read_data("hypatia_path_changes.csv")
    hist = [
        (int(r["measure"].split("_")[2]), int(r["ours"]))
        for r in rows
        if r["measure"].startswith("pairs_with_") and r["measure"].endswith("_changes")
    ]
    summary = [
        r
        for r in rows
        if not (r["measure"].startswith("pairs_with_") and r["measure"].endswith("_changes"))
    ]
    hyp_ax.bar([h[0] for h in hist], [h[1] for h in hist], color=BLUE, label="pairs, ours")
    lookup = {r["measure"]: r for r in rows}
    hyp_ax.axvline(
        float(lookup["median_path_changes"]["ours"]), color=INK, linewidth=1.0, label="median, ours"
    )
    hyp_ax.axvline(
        float(lookup["median_path_changes"]["published"]),
        color=ORANGE,
        linewidth=1.2,
        linestyle="--",
        label="median, published",
    )
    text = "\n".join(
        f"{r['measure'].replace('_', ' ')}: ours {r['ours']}, "
        f"published {r['published'] or '(not given)'}"
        for r in summary
    )
    hyp_ax.text(
        0.98,
        0.97,
        text,
        transform=hyp_ax.transAxes,
        ha="right",
        va="top",
        fontsize=7,
        color=INK_SECONDARY,
    )
    # Room above the bars for the table of the file's other rows.
    hyp_ax.set_ylim(0, 1.6 * max(h[1] for h in hist))
    hyp_ax.set_xlabel("path changes per pair over 200 s")
    hyp_ax.set_ylabel("city pairs (of 4,950)")
    hyp_ax.set_xticks([h[0] for h in hist])
    hyp_ax.set_title("Hypatia's 100 cities (Kassing et al., Fig. 8a)", fontsize=9)
    hyp_ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.12), ncol=3, fontsize=8)

    # LENS Victoria (dish to its Seattle PoP): measured pings against the model's round trip.
    rows = read_data("lens_victoria.csv")
    stats = (
        ("min", "minimum"),
        ("p5", "5th percentile"),
        ("median", "median"),
        ("p95", "95th percentile"),
    )
    x = np.arange(len(stats))
    for k, r in enumerate(rows):
        hour = r["hour_utc"].replace("T", " ")
        dx = (k - (len(rows) - 1) / 2) * 0.18
        lens_ax.plot(
            x + dx,
            [float(r[f"measured_{s}_ms"]) for s, _ in stats],
            marker="o" if k == 0 else "s",
            linestyle="none",
            color=ORANGE,
            label=f"measured, hour from {hour} UTC",
        )
        lens_ax.plot(
            x + dx,
            [float(r[f"model_{s}_ms"]) for s, _ in stats],
            marker="o" if k == 0 else "s",
            linestyle="none",
            color=BLUE,
            label=f"model, hour from {hour} UTC",
        )
    lens_ax.set_xticks(x)
    lens_ax.set_xticklabels([label for _, label in stats])
    lens_ax.set_ylim(bottom=0)
    lens_ax.set_ylabel("round trip, dish to PoP (ms)")
    lens_ax.set_title("LENS dish Victoria to the Seattle PoP: measured against model", fontsize=9)
    lens_ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.1), ncol=2, fontsize=8)
    save(fig, out, "validation.png")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Draw the Phase 3 report's plots.")
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)
    missing = [run for run in RUNS_NEEDED if not (args.results / f"{run}.csv.gz").is_file()]
    if missing:
        print(
            f"error: no run outputs {', '.join(missing)} in {args.results} "
            "(run python/run_phase3.py)",
            file=sys.stderr,
        )
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    m = Measures()
    plot_strategies(m, args.out, "main-best-case", "strategies_best_case.png", "Best case")
    plot_strategies(m, args.out, "main-realistic", "strategies_realistic.png", "Realistic")
    plot_route_over_time(args.results, args.out)
    plot_replan_interval(m, args.out)
    plot_drop_offsets(args.out)
    plot_access_elevation(args.out)
    plot_serving_rules(m, args.out)
    plot_patterns(m, args.out)
    plot_route_lifetime(args.results, args.out)
    plot_threshold(m, args.out)
    plot_validation(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

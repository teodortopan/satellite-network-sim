#!/usr/bin/env python3
"""The Phase 4 report's plots, from the summaries of python/run_phase4.py and
python/compare_failures.py.

Reads only the committed summaries (docs/phase-4-data/: runs.csv, scenarios.csv, measures.csv,
recovery.csv, recovery_summary.csv, starglider.csv, percolation.csv; the columns are defined in
the docstrings of those two scripts), so every figure can be rebuilt from git, and writes to
docs/img/phase-4/:

  lost_time.png            main group: seconds without a route that the baseline had, per
                           scenario and reaction mode, both models (every run, median, 5th to
                           95th percentile).
  lost_time_by_offset.png  main group, single-element scenarios: the planner's lost seconds
                           against the seconds the failure fell after a re-plan instant, next to
                           the wait for the next plan; the other modes' medians.
  restore_and_settle.png   main group: time to restore and time to settle (median and range).
  extra_delay.png          main group: median extra delay over the baseline once a route is
                           back, and at settling.
  detection.png            detection group and main (0.1 s): lost seconds against the
                           detection time, route-satellite and route-laser.
  transient.png            transient group and main (permanent): lost seconds and time to
                           settle against the failure's length.
  planner_variants.png     planner group and main: lost seconds of planner and local-repair
                           under the whole-interval planner, re-planning on a report and the
                           `start` planner.
  compute.png              compute group and main: flooding's lost and loop seconds at two
                           compute times.
  messages.png             main group: report hops (planner, local-repair) and flood sendings
                           (flooding).
  repointing.png           re-pointing group and main (off): extra delay at settling, lost
                           seconds and repair links made against the acquisition time.
  gateway.png              gateway group: lost seconds per mode by the failed gateway's source
                           (real or assumed); extra delay at settling against the next
                           gateway's distance.
  reachability.png         after recovery: share of the pairs that still have a route against
                           the failed share.
  recovery_delay.png       after recovery: median extra delay of the pairs still reachable.
  largest_piece.png        after recovery: the largest piece, per shell where recovery.csv has
                           the columns.
  comparisons.png          StarGlider's 7% against ours per link set, and the percolation
                           curves against the thresholds of endless grids.

A measure that is `none` (or `never`) is left out, never counted as 0; where that matters the
plot says how many runs had none. On a log axis a value of 0 is drawn at the axis floor, the
tick labelled 0. Pairs, instants, shares and seeds are taken from the data.

Usage (inside the CI image, after run_phase4.py and compare_failures.py):

    python3 python/plot_phase4.py [--data DIR] [--out DIR]

--data reads the summaries from another directory (a trial's); starglider.csv and
percolation.csv are read from docs/phase-4-data/ when it lacks them.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FuncFormatter, NullFormatter

import run_phase4 as rp
import snapshot_tools as st

REPO = st.REPO_ROOT
DEFAULT_DATA = REPO / "docs/phase-4-data"
DEFAULT_OUT = REPO / "docs/img/phase-4"

# The palette and style of the Phase 1 to 3 plots.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRID = "#dcdbd7"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
AQUA = "#1baf7a"
GRAY = "#7a7974"
PURPLE = "#8a5cd1"
MODES = rp.ALL_REACTIONS  # planner, local-repair, flooding: always this order and these colours.
MODE_COLOURS = {"planner": BLUE, "local-repair": ORANGE, "flooding": AQUA}
LINK_COLOURS = {"three": BLUE, "plus-grid": ORANGE, "six": AQUA}

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

MODEL_ORDER = ("best-case", "realistic")
MODEL_LABELS = {"best-case": "best case", "realistic": "realistic"}
SCENARIO_ORDER = (
    "route-satellite",
    "access-satellite",
    "route-laser",
    "plane",
    "region",
    "gateway",
)
SINGLE_ELEMENT = ("route-satellite", "access-satellite", "route-laser")
NONE_TEXT = ("none", "never", "")
SPREAD_P5_P95 = "p5-p95"
SPREAD_RANGE = "range"


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


def read_csv(path: Path) -> list[dict]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def num(text: str) -> float:
    """A summary's number; NaN for a measure that does not exist (never 0)."""
    return np.nan if text in NONE_TEXT else float(text)


def values(rows: list[dict], column: str) -> np.ndarray:
    return np.array([num(r[column]) for r in rows], dtype=float)


def ordered(present, order) -> list:
    """The values present, those of `order` first in its order, then the rest sorted."""
    present = set(present)
    return [v for v in order if v in present] + sorted(present - set(order))


def pick(rows: list[dict], **criteria) -> list[dict]:
    return [r for r in rows if all(r[k] == v for k, v in criteria.items())]


def none_count(rows: list[dict], column: str) -> int:
    return sum(1 for r in rows if r[column] in NONE_TEXT)


def scenario_label(scenario: str) -> str:
    return scenario.replace("-", "\n", 1) if scenario in SINGLE_ELEMENT else scenario


def model_label(model: str) -> str:
    return MODEL_LABELS.get(model, model)


def seconds_label(v: float) -> str:
    return f"{v:g} s"


# ---------------------------------------------------------------------------------------
# Log axes with a floor for 0.
# ---------------------------------------------------------------------------------------
def zero_floor(*arrays: np.ndarray) -> float:
    """Where 0 is drawn on a log axis: the decade below the one that holds the smallest
    positive value, so that a labelled decade always lies between 0 and every other value."""
    allv = np.concatenate([a[np.isfinite(a)] for a in arrays] or [np.array([])])
    positive = allv[allv > 0]
    if positive.size == 0:
        return 0.01
    return 10.0 ** (math.floor(math.log10(positive.min())) - 1)


def on_floor(ys: np.ndarray, floor: float | None) -> np.ndarray:
    """0 at the floor. Only a median between 0 and the smallest positive value (of an even
    number of values) can fall below the floor otherwise: it is drawn there too."""
    return ys if floor is None else np.maximum(ys, floor)


def tick(v: float) -> str:
    """A log axis tick: plain digits, or a power of ten when that is long."""
    if 1e-3 <= v < 1e5:
        return f"{v:g}"
    return f"$10^{{{round(math.log10(v))}}}$"


def log_y_with_zero(ax, floor: float) -> None:
    ax.set_yscale("log")
    ax.set_ylim(bottom=floor / 1.8)
    ax.axhline(floor, color=GRAY, linewidth=0.8, linestyle=":", zorder=0)
    ax.yaxis.set_major_formatter(
        FuncFormatter(lambda v, _: "0" if math.isclose(v, floor, rel_tol=1e-9) else tick(v))
    )
    ax.yaxis.set_minor_formatter(NullFormatter())


def log_x_ticks(ax, ticks, label=seconds_label) -> None:
    ax.set_xscale("log")
    ax.set_xticks(list(ticks))
    ax.set_xticklabels([label(t) for t in ticks])
    ax.minorticks_off()


def zero_note(floor: float, unit: str = "s") -> str:
    floor_text = f"{floor:g} {unit}".strip()
    return (
        f"log scale; 0 is drawn at the axis floor (the tick 0, in place of {floor_text}), "
        "and so is a median that falls below it"
    )


# ---------------------------------------------------------------------------------------
# Marks.
# ---------------------------------------------------------------------------------------
def spread(
    ax,
    x: float,
    ys: np.ndarray,
    colour: str,
    *,
    kind: str = SPREAD_RANGE,
    floor: float | None = None,
    points: bool = False,
    width: float = 0.08,
    marker: str = "o",
    filled: bool = True,
    label: str | None = None,
) -> bool:
    """Every value as a faint point (if `points`), the median as a marker and a bar from the
    5th to the 95th percentile or from the least to the greatest. NaN (none) is left out.
    False if nothing is left."""
    ys = ys[np.isfinite(ys)]
    if ys.size == 0:
        return False
    if points:
        dx = np.linspace(-width, width, ys.size) if ys.size > 1 else np.zeros(1)
        ax.scatter(
            x + dx, on_floor(np.sort(ys), floor), s=9, color=colour, alpha=0.28, linewidths=0
        )
    if kind == SPREAD_P5_P95:
        low, high = np.percentile(ys, [5, 95])
    else:
        low, high = ys.min(), ys.max()
    med, low, high = on_floor(np.array([np.median(ys), low, high]), floor)
    ax.errorbar(
        [x],
        [med],
        yerr=[[med - low], [high - med]],
        fmt=marker,
        color=colour,
        markerfacecolor=colour if filled else SURFACE,
        markersize=5.5,
        capsize=2.5,
        elinewidth=1.1,
        label=label,
    )
    return True


def no_runs(ax, x: float) -> None:
    ax.text(
        x,
        0.02,
        "no runs",
        transform=ax.get_xaxis_transform(),
        ha="center",
        va="bottom",
        fontsize=6,
        color=INK_SECONDARY,
        rotation=90,
    )


def spread_proxies(ax, kind: str, points: bool) -> None:
    """Legend entries that say what the marks are."""
    if points:
        ax.scatter([], [], s=9, color=GRAY, alpha=0.4, label="one run")
    bar = "5th to 95th percentile" if kind == SPREAD_P5_P95 else "least to greatest"
    ax.errorbar([], [], yerr=[], fmt="o", color=GRAY, capsize=2.5, label=f"median; bar: {bar}")


def mode_offsets(modes, step: float = 0.26) -> dict[str, float]:
    return {m: (k - (len(modes) - 1) / 2) * step for k, m in enumerate(modes)}


# ---------------------------------------------------------------------------------------
# The data.
# ---------------------------------------------------------------------------------------
class Data:
    """The summaries. Run names repeat between groups (main and gateway share the gateway
    runs' names), so a run is (group, run)."""

    def __init__(self, data_dir: Path) -> None:
        self.dir = data_dir
        self.runs = {(r["group"], r["run"]): r for r in self.read("runs.csv")}
        self.scenarios = {(r["group"], r["run"]): r for r in self.read("scenarios.csv")}
        self.measures = self.read("measures.csv")
        for m in self.measures:
            m["_run"] = self.runs[(m["group"], m["run"])]
            m["_scenario"] = self.scenarios[(m["group"], m["run"])]
        self.recovery = self.read("recovery.csv")
        self.recovery_summary = self.read("recovery_summary.csv")
        self.starglider = self.read("starglider.csv", fallback=True)
        self.percolation = self.read("percolation.csv", fallback=True)

    def read(self, name: str, fallback: bool = False) -> list[dict]:
        path = self.dir / name
        if fallback and not path.is_file():
            path = DEFAULT_DATA / name
        return read_csv(path)

    def group(self, group: str, **criteria) -> list[dict]:
        return pick([m for m in self.measures if m["group"] == group], **criteria)

    def models(self, rows: list[dict]) -> list[str]:
        return ordered({r["model"] for r in rows}, MODEL_ORDER)

    def scenario_list(self, rows: list[dict]) -> list[str]:
        return ordered({r["scenario"] for r in rows}, SCENARIO_ORDER)


def runs_text(rows: list[dict]) -> str:
    return f"{len({(r['group'], r['run']) for r in rows})} runs"


# ---------------------------------------------------------------------------------------
def scenario_panels(
    d: Data,
    out: Path,
    rows: list[dict],
    measures: list[tuple[str, str, bool]],
    *,
    name: str,
    title: str,
    kind: str = SPREAD_RANGE,
    points: bool = False,
    note_none: bool = False,
) -> None:
    """Rows of panels, one per (column, y label, log); a column per model; per scenario the
    three modes side by side."""
    models = d.models(rows)
    scenarios = d.scenario_list(rows)
    offsets = mode_offsets(MODES)
    fig, axes = plt.subplots(
        len(measures),
        len(models),
        figsize=(6.6 * len(models), 3.8 * len(measures) + 0.6),
        squeeze=False,
        sharey="row",
    )
    notes = []
    for row, (column, ylabel, log) in enumerate(measures):
        floor = zero_floor(values(rows, column)) if log else None
        for col, model in enumerate(models):
            ax = axes[row][col]
            for i, sc in enumerate(scenarios):
                drawn = False
                for mode in MODES:
                    ys = values(pick(rows, model=model, scenario=sc, reaction=mode), column)
                    drawn |= spread(
                        ax,
                        i + offsets[mode],
                        ys,
                        MODE_COLOURS[mode],
                        kind=kind,
                        floor=floor,
                        points=points,
                        label=mode if (row, col, i) == (0, 0, 0) else None,
                    )
                if not drawn:
                    no_runs(ax, i)
            if log:
                log_y_with_zero(ax, floor)
            ax.set_xticks(range(len(scenarios)))
            ax.set_xticklabels([scenario_label(s) for s in scenarios], fontsize=8)
            ax.set_xlim(-0.6, len(scenarios) - 0.4)
            if row == 0:
                ax.set_title(f"{model_label(model)} ({runs_text(pick(rows, model=model))})")
            if col == 0:
                ax.set_ylabel(ylabel + (" (log)" if log else ""))
        if log:
            notes.append(f"{plain(ylabel)}: {zero_note(floor, ylabel_unit(ylabel))}")
        if note_none:
            counts = ", ".join(
                f"{mode} {none_count(pick(rows, reaction=mode), column)} of "
                f"{len(pick(rows, reaction=mode))}"
                for mode in MODES
            )
            notes.append(f"{plain(ylabel)}, none (left out): {counts}")
    spread_proxies(axes[0][0], kind, points)
    legend_below(fig, axes[0][0], ncol=5)
    fig.suptitle(title + ("\n" + "\n".join(notes) if notes else ""), fontsize=9)
    save(fig, out, name)


def plain(ylabel: str) -> str:
    """An axis label as words for a note: one line, no unit."""
    return re.sub(r" \((s|ms|log|count)\)", "", ylabel.replace("\n", " "))


def ylabel_unit(ylabel: str) -> str:
    return "ms" if "(ms)" in ylabel else "" if "(s)" not in ylabel else "s"


# ---------------------------------------------------------------------------------------
def plot_lost_time(d: Data, out: Path) -> None:
    rows = d.group("main")
    scenario_panels(
        d,
        out,
        rows,
        [("lost_s", "seconds without a route\nthat the baseline had (s)", True)],
        name="lost_time.png",
        title="Main group: time lost to the failure, per scenario and reaction mode "
        "(permanent failure, the defaults of spec 4.9)",
        kind=SPREAD_P5_P95,
        points=True,
    )


# ---------------------------------------------------------------------------------------
def plot_lost_by_offset(d: Data, out: Path) -> None:
    rows = [r for r in d.group("main") if r["scenario"] in SINGLE_ELEMENT]
    scenarios = d.scenario_list(rows)
    models = d.models(rows)
    markers = {"best-case": "o", "realistic": "s"}
    shifts = {m: (k - (len(models) - 1) / 2) * 0.25 for k, m in enumerate(models)}
    floor = zero_floor(values(rows, "lost_s"))
    fig, axes = plt.subplots(
        1, len(scenarios), figsize=(5.4 * len(scenarios), 4.8), sharey=True, squeeze=False
    )
    for ax, sc in zip(axes[0], scenarios, strict=True):
        wait = np.linspace(0, rp.REPLAN_S - rp.STEP_S, 57)  # Offsets are whole steps.
        ax.plot(
            wait,
            rp.REPLAN_S - wait,
            color=GRAY,
            linestyle="--",
            linewidth=1.0,
            label=f"{rp.REPLAN_S} s minus the offset (the wait for the next plan)",
        )
        for model in models:
            rs = pick(rows, scenario=sc, model=model, reaction="planner")
            ax.scatter(
                values(rs, "replan_offset_s") + shifts[model],
                on_floor(values(rs, "lost_s"), floor),
                s=18,
                marker=markers.get(model, "D"),
                color=MODE_COLOURS["planner"],
                facecolors=MODE_COLOURS["planner"] if model == "best-case" else SURFACE,
                linewidths=1.0,
                alpha=0.8,
                label=f"planner, one run, {model_label(model)}",
            )
        for mode in MODES[1:]:
            ys = values(pick(rows, scenario=sc, reaction=mode), "lost_s")
            ys = ys[np.isfinite(ys)]
            if ys.size:
                ax.axhline(
                    on_floor(np.array([np.median(ys)]), floor)[0],
                    color=MODE_COLOURS[mode],
                    linewidth=1.4,
                    label=f"{mode}, median of every run (both models)",
                )
        log_y_with_zero(ax, floor)
        ax.set_xlim(-0.8, rp.REPLAN_S + 0.3)
        ax.set_xlabel("seconds the failure fell after a re-plan instant")
        ax.set_title(f"{sc} ({runs_text(pick(rows, scenario=sc))})")
    axes[0][0].set_ylabel("seconds without a route that the baseline had (log)")
    handles, labels = {}, []
    for ax in axes[0]:
        for h, lab in zip(*ax.get_legend_handles_labels(), strict=True):
            if lab not in handles:
                handles[lab] = h
                labels.append(lab)
    fig.legend(
        [handles[lab] for lab in labels],
        labels,
        loc="upper center",
        bbox_to_anchor=(0.5, -0.02),
        ncol=3,
    )
    fig.suptitle(
        "Main group, single-element scenarios: the planner's lost time against where the "
        f"failure falls in the {rp.REPLAN_S} s re-plan interval\n{zero_note(floor)}",
        fontsize=9,
    )
    save(fig, out, "lost_time_by_offset.png")


# ---------------------------------------------------------------------------------------
def plot_restore_settle(d: Data, out: Path) -> None:
    scenario_panels(
        d,
        out,
        d.group("main"),
        [
            ("restore_s", "time to restore (s)", True),
            ("settle_s", "time to settle (s)", True),
        ],
        name="restore_and_settle.png",
        title="Main group: time from the failure until a route is back (restore) and until "
        "the route is the full-knowledge planner's and stays so (settle)",
        note_none=True,
    )


def plot_extra_delay(d: Data, out: Path) -> None:
    scenario_panels(
        d,
        out,
        d.group("main"),
        [
            (
                "extra_median_ms",
                "median extra delay over the\nbaseline once a route is back (ms)",
                False,
            ),
            ("extra_settled_ms", "extra delay over the\nbaseline at settling (ms)", False),
        ],
        name="extra_delay.png",
        title="Main group: extra one-way delay over the no-failure baseline at the same "
        "instant, while both are delivered",
        note_none=True,
    )


# ---------------------------------------------------------------------------------------
def with_setting(d: Data, group: str, setting: str, scenarios, main_value=None) -> list[dict]:
    """The group's rows of the scenarios, plus main's, each carrying `_x`: its run's
    `setting` from runs.csv."""
    rows = [r for r in d.group(group) if r["scenario"] in scenarios]
    rows += [r for r in d.group("main") if r["scenario"] in scenarios]
    for r in rows:
        r["_x"] = (
            main_value
            if (r["group"] == "main" and main_value is not None)
            else num(r["_run"][setting])
        )
    return rows


def plot_detection(d: Data, out: Path) -> None:
    det_scenarios = {r["scenario"] for r in d.group("detection")}
    rows = with_setting(d, "detection", "detection_s", det_scenarios)
    scenarios = d.scenario_list(rows)
    models = d.models(pick(rows, group="detection"))
    xs = sorted({r["_x"] for r in rows})
    floor = zero_floor(values(rows, "lost_s"))
    default = sorted({num(r["_run"]["detection_s"]) for r in d.group("main")})
    fig, axes = plt.subplots(
        len(scenarios),
        len(models),
        figsize=(6.4 * len(models), 3.9 * len(scenarios)),
        sharex=True,
        sharey=True,
        squeeze=False,
    )
    for row, sc in enumerate(scenarios):
        for col, model in enumerate(models):
            ax = axes[row][col]
            for k, mode in enumerate(MODES):
                med, low, high, at = [], [], [], []
                for x in xs:
                    rs = pick(rows, scenario=sc, model=model, reaction=mode)
                    ys = values([r for r in rs if r["_x"] == x], "lost_s")
                    ys = ys[np.isfinite(ys)]
                    if ys.size:
                        at.append(x * (1 + 0.06 * (k - 1)))
                        m3 = on_floor(np.array([np.median(ys), ys.min(), ys.max()]), floor)
                        med.append(m3[0])
                        low.append(m3[1])
                        high.append(m3[2])
                if not at:
                    continue
                med, low, high = np.array(med), np.array(low), np.array(high)
                ax.errorbar(
                    at,
                    med,
                    yerr=[med - low, high - med],
                    marker="o",
                    markersize=4.5,
                    capsize=2.5,
                    color=MODE_COLOURS[mode],
                    label=mode if (row, col) == (0, 0) else None,
                )
            if not pick(rows, scenario=sc, model=model):
                ax.text(
                    0.5, 0.5, "no runs", transform=ax.transAxes, ha="center", color=INK_SECONDARY
                )
            log_y_with_zero(ax, floor)
            log_x_ticks(ax, xs)
            ax.set_title(
                f"{sc}, {model_label(model)} ({runs_text(pick(rows, scenario=sc, model=model))})"
            )
            if col == 0:
                ax.set_ylabel("seconds without a route\nthat the baseline had (log)")
            if row == len(scenarios) - 1:
                ax.set_xlabel("detection time (log scale)")
    spread_proxies(axes[0][0], SPREAD_RANGE, False)
    legend_below(fig, axes[0][0], ncol=4)
    fig.suptitle(
        "Detection group, with the main group at its default "
        f"({', '.join(seconds_label(v) for v in default)}): lost time against the detection "
        f"time (modes drawn slightly apart along x)\n{zero_note(floor)}",
        fontsize=9,
    )
    save(fig, out, "detection.png")


# ---------------------------------------------------------------------------------------
PERMANENT = math.inf


def categorical_panels(
    d: Data,
    out: Path,
    rows: list[dict],
    panels: list[tuple[str, str]],
    measures: list[tuple[str, str, bool]],
    categories: list,
    category_label,
    *,
    name: str,
    title: str,
    xlabel: str,
    note_none: tuple[str, ...] = (),
) -> None:
    """Rows of panels per measure, one column per (scenario, model); x: the rows' `_x`."""
    offsets = mode_offsets(MODES, 0.22)
    fig, axes = plt.subplots(
        len(measures),
        len(panels),
        figsize=(4.3 * len(panels), 3.6 * len(measures) + 0.6),
        squeeze=False,
        sharey="row",
    )
    notes = []
    for row, (column, ylabel, log) in enumerate(measures):
        floor = zero_floor(values(rows, column)) if log else None
        for col, (sc, model) in enumerate(panels):
            ax = axes[row][col]
            for i, x in enumerate(categories):
                for mode in MODES:
                    rs = [
                        r
                        for r in pick(rows, scenario=sc, model=model, reaction=mode)
                        if r["_x"] == x
                    ]
                    spread(
                        ax,
                        i + offsets[mode],
                        values(rs, column),
                        MODE_COLOURS[mode],
                        floor=floor,
                        points=True,
                        width=0.05,
                        label=mode if (row, col, i) == (0, 0, 0) else None,
                    )
            if log:
                log_y_with_zero(ax, floor)
            ax.set_xticks(range(len(categories)))
            ax.set_xticklabels([category_label(c) for c in categories], fontsize=8)
            ax.set_xlim(-0.6, len(categories) - 0.4)
            if row == 0:
                ax.set_title(
                    f"{sc}, {model_label(model)}\n"
                    f"({runs_text(pick(rows, scenario=sc, model=model))})",
                    fontsize=9,
                )
            if row == len(measures) - 1:
                ax.set_xlabel(xlabel)
            if col == 0:
                ax.set_ylabel(ylabel + (" (log)" if log else ""))
        if log:
            notes.append(f"{plain(ylabel)}: {zero_note(floor, ylabel_unit(ylabel))}")
        if column in note_none:
            counts = ", ".join(
                f"{m} {none_count(pick(rows, reaction=m), column)} of {len(pick(rows, reaction=m))}"
                for m in MODES
            )
            notes.append(f"{plain(ylabel)}, none (left out): {counts}")
    spread_proxies(axes[0][0], SPREAD_RANGE, True)
    legend_below(fig, axes[0][0], ncol=5)
    fig.suptitle(title + ("\n" + "\n".join(notes) if notes else ""), fontsize=9)
    save(fig, out, name)


def panels_of(d: Data, rows: list[dict]) -> list[tuple[str, str]]:
    present = {(r["scenario"], r["model"]) for r in rows}
    return [(s, m) for s in d.scenario_list(rows) for m in d.models(rows) if (s, m) in present]


def plot_transient(d: Data, out: Path) -> None:
    scenarios = {r["scenario"] for r in d.group("transient")}
    rows = with_setting(d, "transient", "length_s", scenarios, main_value=PERMANENT)
    lengths = sorted({r["_x"] for r in rows})
    categorical_panels(
        d,
        out,
        rows,
        panels_of(d, pick(rows, group="transient")),
        [
            ("lost_s", "seconds without a route\nthat the baseline had (s)", True),
            ("settle_s", "time to settle (s)", True),
        ],
        lengths,
        lambda v: "permanent\n(main)" if v == PERMANENT else seconds_label(v),
        name="transient.png",
        title="Transient group, with the main group's permanent failures: lost time and time "
        "to settle against the failure's length",
        xlabel="how long the failure lasts",
        note_none=("settle_s",),
    )


# ---------------------------------------------------------------------------------------
def planner_variant(r: dict) -> str:
    run = r["_run"]
    if run["planner"] != "whole-interval":
        return f"{run['planner']} planner"
    if run["replan_on_report"] == "on":
        return "whole-interval, re-plan on report"
    return "whole-interval (main)"


def plot_planner_variants(d: Data, out: Path) -> None:
    modes = ("planner", "local-repair")
    rows = [r for r in d.group("main") + d.group("planner") if r["reaction"] in modes]
    variants = ordered(
        {planner_variant(r) for r in rows},
        ("whole-interval (main)", "whole-interval, re-plan on report", "start planner"),
    )
    markers = dict(zip(variants, ("o", "^", "s", "D", "v"), strict=False))
    models = d.models(rows)
    scenarios = d.scenario_list(rows)
    series = [(m, v) for m in modes for v in variants]
    step = 0.8 / len(series)
    floor = zero_floor(values(rows, "lost_s"))
    fig, axes = plt.subplots(
        1, len(models), figsize=(7.5 * len(models), 5), sharey=True, squeeze=False
    )
    for col, model in enumerate(models):
        ax = axes[0][col]
        for i, sc in enumerate(scenarios):
            drawn = False
            for k, (mode, variant) in enumerate(series):
                rs = [
                    r
                    for r in pick(rows, model=model, scenario=sc, reaction=mode)
                    if planner_variant(r) == variant
                ]
                drawn |= spread(
                    ax,
                    i + (k - (len(series) - 1) / 2) * step,
                    values(rs, "lost_s"),
                    MODE_COLOURS[mode],
                    floor=floor,
                    marker=markers[variant],
                    filled=k % len(variants) == 0,
                    label=f"{mode}, {variant}" if (col, i) == (0, 0) else None,
                )
            if not drawn:
                no_runs(ax, i)
        log_y_with_zero(ax, floor)
        ax.set_xticks(range(len(scenarios)))
        ax.set_xticklabels([scenario_label(s) for s in scenarios], fontsize=8)
        ax.set_xlim(-0.6, len(scenarios) - 0.4)
        ax.set_title(f"{model_label(model)} ({runs_text(pick(rows, model=model))})")
    axes[0][0].set_ylabel("seconds without a route that the baseline had (log)")
    spread_proxies(axes[0][0], SPREAD_RANGE, False)
    legend_below(fig, axes[0][0], ncol=4)
    fig.suptitle(
        "Planner group against main: lost time of planner and local-repair under three "
        f"planner settings\n{zero_note(floor)}",
        fontsize=9,
    )
    save(fig, out, "planner_variants.png")


# ---------------------------------------------------------------------------------------
def plot_compute(d: Data, out: Path) -> None:
    rows = pick(d.group("main") + d.group("compute"), reaction="flooding")
    computes = sorted({num(r["_run"]["compute_s"]) for r in rows}, reverse=True)
    style = dict(zip(computes, (("o", True), ("D", False), ("s", False)), strict=False))
    models = d.models(rows)
    scenarios = d.scenario_list(rows)
    step = 0.3
    measures = [
        ("lost_s", "seconds without a route\nthat the baseline had (log)"),
        ("loop_s", "seconds in a loop (log)"),
    ]
    fig, axes = plt.subplots(
        2, len(models), figsize=(6.6 * len(models), 8), sharey="row", squeeze=False
    )
    notes = []
    labelled = set()
    for row, (column, ylabel) in enumerate(measures):
        floor = zero_floor(values(rows, column))
        notes.append(f"{plain(ylabel)}: {zero_note(floor)}")
        for col, model in enumerate(models):
            ax = axes[row][col]
            for i, sc in enumerate(scenarios):
                drawn = False
                for k, c in enumerate(computes):
                    rs = [
                        r
                        for r in pick(rows, model=model, scenario=sc)
                        if num(r["_run"]["compute_s"]) == c
                    ]
                    marker, filled = style[c]
                    label = None
                    if rs and (row, col) == (0, 0) and c not in labelled:
                        labelled.add(c)
                        groups = "/".join(sorted({r["group"] for r in rs}))
                        label = f"flooding, compute {seconds_label(c)} ({groups} group)"
                    drawn |= spread(
                        ax,
                        i + (k - (len(computes) - 1) / 2) * step,
                        values(rs, column),
                        MODE_COLOURS["flooding"],
                        floor=floor,
                        points=True,
                        width=0.05,
                        marker=marker,
                        filled=filled,
                        label=label,
                    )
                if not drawn:
                    no_runs(ax, i)
            log_y_with_zero(ax, floor)
            ax.set_xticks(range(len(scenarios)))
            ax.set_xticklabels([scenario_label(s) for s in scenarios], fontsize=8)
            ax.set_xlim(-0.6, len(scenarios) - 0.4)
            if row == 0:
                ax.set_title(f"{model_label(model)} ({runs_text(pick(rows, model=model))})")
            if col == 0:
                ax.set_ylabel(ylabel)
    spread_proxies(axes[0][0], SPREAD_RANGE, True)
    legend_below(fig, axes[0][0], ncol=4)
    fig.suptitle(
        "Compute group against main: flooding with two compute times (the time a "
        "node takes to work out new tables)\n" + "\n".join(notes),
        fontsize=9,
    )
    save(fig, out, "compute.png")


# ---------------------------------------------------------------------------------------
def plot_messages(d: Data, out: Path) -> None:
    scenario_panels(
        d,
        out,
        d.group("main"),
        [("messages", "messages per run", True)],
        name="messages.png",
        title="Main group: messages. planner and local-repair: the hops of every report sent "
        "to the ground (the same reports in both); flooding: every sending of every fact on "
        "a link",
        points=True,
    )


# ---------------------------------------------------------------------------------------
REPOINT_OFF = -1.0


def plot_repointing(d: Data, out: Path) -> None:
    scenarios = {r["scenario"] for r in d.group("repointing")}
    rows = with_setting(d, "repointing", "acquisition_s", scenarios, main_value=REPOINT_OFF)
    acquisitions = sorted({r["_x"] for r in rows})
    panels = panels_of(d, pick(rows, group="repointing"))

    def label(v):
        return "off\n(main)" if v == REPOINT_OFF else seconds_label(v)

    measures = [
        ("extra_settled_ms", "extra delay over the\nbaseline at settling (ms)", False),
        ("lost_s", "seconds without a route\nthat the baseline had (s)", True),
    ]
    offsets = mode_offsets(MODES, 0.22)
    fig, axes = plt.subplots(
        3, len(panels), figsize=(3.9 * len(panels), 11), squeeze=False, sharey="row"
    )
    notes = []
    for row, (column, ylabel, log) in enumerate(measures):
        floor = zero_floor(values(rows, column)) if log else None
        for col, (sc, model) in enumerate(panels):
            ax = axes[row][col]
            for i, x in enumerate(acquisitions):
                for mode in MODES:
                    rs = [
                        r
                        for r in pick(rows, scenario=sc, model=model, reaction=mode)
                        if r["_x"] == x
                    ]
                    spread(
                        ax,
                        i + offsets[mode],
                        values(rs, column),
                        MODE_COLOURS[mode],
                        floor=floor,
                        points=True,
                        width=0.05,
                        label=mode if (row, col, i) == (0, 0, 0) else None,
                    )
            if log:
                log_y_with_zero(ax, floor)
            ax.set_xticks(range(len(acquisitions)))
            ax.set_xticklabels([])
            if row == 0:
                ax.set_title(
                    f"{sc}, {model_label(model)}\n"
                    f"({runs_text(pick(rows, scenario=sc, model=model))})",
                    fontsize=9,
                )
            if col == 0:
                ax.set_ylabel(ylabel + (" (log)" if log else ""))
        if log:
            notes.append(f"{plain(ylabel)}: {zero_note(floor)}")
        counts = ", ".join(
            f"{m} {none_count(pick(rows, reaction=m), column)} of {len(pick(rows, reaction=m))}"
            for m in MODES
        )
        notes.append(f"{plain(ylabel)}, none (left out): {counts}")
    # Repair links made: one value per run (runs.csv), not per mode.
    run_rows = {}
    for r in rows:
        run_rows[(r["group"], r["run"])] = (r["scenario"], r["model"], r["_x"], r["_run"])
    for col, (sc, model) in enumerate(panels):
        ax = axes[2][col]
        for i, x in enumerate(acquisitions):
            made = np.array(
                [
                    num(run["repair_links_made"])
                    for s, m, rx, run in run_rows.values()
                    if (s, m, rx) == (sc, model, x)
                ]
            )
            spread(
                ax,
                i,
                made,
                INK,
                points=True,
                width=0.08,
                label="repair links made per run" if (col, i) == (0, 0) else None,
            )
        ax.set_xticks(range(len(acquisitions)))
        ax.set_xticklabels([label(a) for a in acquisitions], fontsize=8)
        ax.set_xlabel("re-pointing: acquisition time")
        ax.set_ylim(bottom=0)
        if col == 0:
            ax.set_ylabel("repair links made per run")
    spread_proxies(axes[0][0], SPREAD_RANGE, True)
    handles, labels = axes[0][0].get_legend_handles_labels()
    h2, l2 = axes[2][0].get_legend_handles_labels()
    fig.legend(
        handles + h2[:1], labels + l2[:1], loc="upper center", bbox_to_anchor=(0.5, -0.01), ncol=6
    )
    fig.suptitle(
        "Re-pointing group against main (re-pointing off): what re-pointing lasers "
        "changes, against the acquisition time\n" + "\n".join(notes),
        fontsize=9,
    )
    save(fig, out, "repointing.png")


# ---------------------------------------------------------------------------------------
def from_source(rows: list[dict], source: str) -> list[dict]:
    """The rows whose failed gateway is `source` (real or assumed)."""
    return [r for r in rows if r["_scenario"]["gateway_source"] == source]


def plot_gateway(d: Data, out: Path) -> None:
    rows = d.group("gateway", scenario="gateway")
    sources = ordered({r["_scenario"]["gateway_source"] for r in rows}, ("real", "assumed"))
    offsets = mode_offsets(MODES, 0.22)
    floor = zero_floor(values(rows, "lost_s"))
    fig, (lost_ax, delay_ax) = plt.subplots(1, 2, figsize=(13, 5))
    for i, source in enumerate(sources):
        for mode in MODES:
            rs = from_source(pick(rows, reaction=mode), source)
            spread(
                lost_ax,
                i + offsets[mode],
                values(rs, "lost_s"),
                MODE_COLOURS[mode],
                floor=floor,
                points=True,
                width=0.05,
                label=mode if i == 0 else None,
            )
    log_y_with_zero(lost_ax, floor)
    lost_ax.set_xticks(range(len(sources)))
    lost_ax.set_xticklabels(
        [f"{s} gateway failed\n({runs_text(from_source(rows, s))})" for s in sources]
    )
    lost_ax.set_xlim(-0.6, len(sources) - 0.4)
    lost_ax.set_ylabel("seconds without a route that the baseline had (log)")
    lost_ax.set_title("Lost time, by the failed gateway's source")
    spread_proxies(lost_ax, SPREAD_RANGE, True)
    markers = {"real": "o", "assumed": "^"}
    for mode in MODES:
        for source in sources:
            rs = from_source(pick(rows, reaction=mode), source)
            xs = np.array([num(r["_scenario"]["next_gateway_km"]) for r in rs])
            ys = values(rs, "extra_settled_ms")
            keep = np.isfinite(ys) & np.isfinite(xs)
            delay_ax.scatter(
                xs[keep],
                ys[keep],
                s=60 - 22 * MODES.index(mode),  # Smaller on top: stacked points show.
                marker=markers.get(source, "s"),
                color=MODE_COLOURS[mode],
                alpha=0.7,
                facecolors=MODE_COLOURS[mode] if source == "real" else SURFACE,
                label=f"{mode}, {source} gateway failed",
            )
    delay_ax.set_xlabel("distance from the failed gateway to the next operational one (km)")
    delay_ax.set_ylabel("extra delay over the baseline at settling (ms)")
    delay_ax.set_title("Extra delay at settling against the next gateway's distance")
    delay_ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.12), ncol=2, fontsize=8)
    lost_ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.16), ncol=3, fontsize=8)
    counts = ", ".join(
        f"{m} {none_count(pick(rows, reaction=m), 'extra_settled_ms')} of "
        f"{len(pick(rows, reaction=m))}"
        for m in MODES
    )
    fig.suptitle(
        "Gateway group (realistic): the gateway the route uses fails\n"
        f"{zero_note(floor)}; extra delay at settling none (left out): {counts}",
        fontsize=9,
    )
    save(fig, out, "gateway.png")


# ---------------------------------------------------------------------------------------
# After recovery.
# ---------------------------------------------------------------------------------------
DRAW_COLOURS = {"random-satellites": BLUE, "random-lasers": ORANGE, "attack": PURPLE}
DRAW_LABELS = {
    "random-satellites": "random satellites",
    "random-lasers": "random laser links",
    "attack": "attack (busiest satellites first)",
}


def recovery_key(r: dict) -> tuple[str, str, str, str]:
    return (r["group"], r["scenario"], r["pattern"], r["repoint"])


def recovery_series(d: Data) -> list[tuple[str, str, str, str]]:
    keys = []
    for r in d.recovery_summary:
        if recovery_key(r) not in keys:
            keys.append(recovery_key(r))
    return keys


def recovery_style(key, k: int = 0) -> dict:
    _, scenario, pattern, repoint = key
    label = DRAW_LABELS.get(scenario, scenario)
    colour = DRAW_COLOURS.get(scenario, GRAY)
    if pattern != "three":
        label += f", pattern {pattern}"
        colour = {"plus-grid": AQUA, "six": GRAY}.get(pattern, INK)
    else:
        label += ", pattern three"
    if repoint == "on":
        label += ", re-pointing on"
    # Earlier lines wider, so lines that coincide still show as bands.
    return {
        "label": label,
        "color": colour,
        "linestyle": "--" if repoint == "on" else "-",
        "linewidth": max(3.4 - 0.4 * k, 1.0),
    }


def share_label(v: float) -> str:
    return f"{100 * v:g}%"


def recovery_axis(ax, shares, ylabel: str) -> None:
    log_x_ticks(ax, shares, share_label)
    ax.set_xlabel("failed share (log scale)")
    ax.set_ylabel(ylabel)


def recovery_runs_note(d: Data) -> str:
    runs = sorted({int(r["runs"]) for r in d.recovery_summary})
    pairs = sorted({r["pairs"] for r in d.recovery})
    return (
        f"{' or '.join(map(str, runs))} runs per point (seeds; the attack: instants); "
        f"{' or '.join(pairs)} pairs of Hypatia's 100 cities, best case, every failure "
        "old news"
    )


def summary_lines(d: Data, ax, measure: str, floor: float | None = None) -> None:
    for k, key in enumerate(recovery_series(d)):
        rs = sorted(
            [r for r in d.recovery_summary if recovery_key(r) == key],
            key=lambda r: float(r["share"]),
        )
        x = values(rs, "share")
        mean, low, high = (values(rs, f"{measure}_{s}") for s in ("mean", "p2_5", "p97_5"))
        keep = np.isfinite(mean)
        style = recovery_style(key, k)
        n = values(rs, f"{measure}_n")
        runs = values(rs, "runs")
        if np.any(n < runs):
            style["label"] += f" (exists in {int(n.sum())} of {int(runs.sum())} runs)"
        ax.plot(x[keep], on_floor(mean[keep], floor), marker="o", markersize=3.5, **style)
        ax.fill_between(
            x[keep],
            on_floor(low[keep], floor),
            on_floor(high[keep], floor),
            color=style["color"],
            alpha=0.12,
            linewidth=0,
        )


def plot_reachability(d: Data, out: Path) -> None:
    shares = sorted({float(r["share"]) for r in d.recovery_summary})
    fig, ax = plt.subplots(figsize=(9, 5.5))
    summary_lines(d, ax, "reach_reference")
    recovery_axis(ax, shares, "share of the pairs that still have a route")
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.14), ncol=2, fontsize=8)
    ax.set_title(
        "After recovery: pairs that still have a route (every actor knowing "
        f"everything)\nline: mean; band: 2.5th to 97.5th percentile. "
        f"{recovery_runs_note(d)}",
        fontsize=9,
    )
    save(fig, out, "reachability.png")


def plot_recovery_delay(d: Data, out: Path) -> None:
    shares = sorted({float(r["share"]) for r in d.recovery_summary})
    floor = zero_floor(
        *(values(d.recovery_summary, f"extra_median_ms_{s}") for s in ("mean", "p2_5", "p97_5"))
    )
    fig, ax = plt.subplots(figsize=(9, 5.5))
    summary_lines(d, ax, "extra_median_ms", floor)
    log_y_with_zero(ax, floor)
    recovery_axis(ax, shares, "median extra delay of the pairs still reachable (ms, log)")
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.14), ncol=2, fontsize=8)
    ax.set_title(
        "After recovery: median extra one-way delay over nothing failed, over the "
        "pairs reachable in both\nline: mean over runs; band: 2.5th to 97.5th "
        f"percentile. {zero_note(floor, 'ms')}.\n{recovery_runs_note(d)}",
        fontsize=9,
    )
    save(fig, out, "recovery_delay.png")


def plot_largest_piece(d: Data, out: Path) -> None:
    shares = sorted({float(r["share"]) for r in d.recovery_summary})
    prefix = "largest_piece_share_"
    shell_columns = [c for c in d.recovery[0] if c.startswith(prefix)]
    columns = shell_columns or ["largest_piece_share"]
    fig, axes = plt.subplots(
        1, len(columns), figsize=(4.6 * len(columns), 4.8), sharey=True, squeeze=False
    )
    for ax, column in zip(axes[0], columns, strict=True):
        for k, key in enumerate(recovery_series(d)):
            rs = [r for r in d.recovery if recovery_key(r) == key]
            xs, mean, low, high = [], [], [], []
            for share in sorted({float(r["share"]) for r in rs}):
                ys = values([r for r in rs if float(r["share"]) == share], column)
                ys = ys[np.isfinite(ys)]
                if ys.size:
                    xs.append(share)
                    mean.append(ys.mean())
                    p_low, p_high = np.percentile(ys, [2.5, 97.5])
                    low.append(p_low)
                    high.append(p_high)
            style = recovery_style(key, k)
            ax.plot(xs, mean, marker="o", markersize=3.5, **style)
            ax.fill_between(xs, low, high, color=style["color"], alpha=0.12, linewidth=0)
        shell = column.removeprefix(prefix) if column != "largest_piece_share" else "all shells"
        ax.set_title(f"shell {shell.replace('deg', ' deg')}")
        recovery_axis(ax, shares, "largest piece (share of the working satellites)")
        ax.set_ylim(0, 1.03)
    legend_below(fig, axes[0][0], ncol=3)
    what = (
        "per shell (lasers never join two shells, so the overall piece is one shell)"
        if shell_columns
        else "over all shells"
    )
    fig.suptitle(
        "After recovery: the largest piece joined by working lasers, "
        + what
        + "\nline: mean over runs; band: 2.5th to 97.5th percentile (from "
        f"recovery.csv). {recovery_runs_note(d)}",
        fontsize=9,
    )
    save(fig, out, "largest_piece.png")


# ---------------------------------------------------------------------------------------
def plot_comparisons(d: Data, out: Path) -> None:
    fig, (share_ax, ratio_ax, perc_ax) = plt.subplots(
        1, 3, figsize=(17, 5.2), gridspec_kw={"width_ratios": [1, 1.1, 1.5]}
    )
    rows = d.starglider
    x = np.arange(len(rows))
    bars = (
        ("share_over_20pct", "over 20% more delay, of all draws", BLUE),
        ("share_over_20pct_of_repaired", "over 20% more delay, of the repaired", AQUA),
        ("share_no_backup", "no backup", GRAY),
    )
    width = 0.8 / len(bars)
    for k, (column, label, colour) in enumerate(bars):
        ys = values(rows, column)
        share_ax.bar(
            x + (k - (len(bars) - 1) / 2) * width, ys, width=width * 0.95, color=colour, label=label
        )
    for value in sorted({num(r["published_share"]) for r in rows}):
        share_ax.axhline(
            value,
            color=ORANGE,
            linestyle="--",
            linewidth=1.2,
            label=f"published (StarGlider): {value:g}",
        )
    share_ax.set_xticks(x)
    share_ax.set_xticklabels([f"{r['links']}\n({r['draws']} draws)" for r in rows])
    share_ax.set_xlabel("link set")
    share_ax.set_ylabel("share of draws")
    share_ax.set_title(
        "StarGlider: one laser link of the route fails;\nthe local backup's "
        "delay against the route's",
        fontsize=9,
    )
    share_ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.2), ncol=1, fontsize=8)

    quantiles = (
        ("ratio_min", "min"),
        ("ratio_median", "median"),
        ("ratio_p90", "90th"),
        ("ratio_p95", "95th"),
        ("ratio_p99", "99th"),
        ("ratio_max", "max"),
    )
    qx = np.arange(len(quantiles))
    for k, r in enumerate(rows):
        ratio_ax.plot(
            qx + (k - (len(rows) - 1) / 2) * 0.12,
            [num(r[c]) for c, _ in quantiles],
            marker="o",
            linestyle="none",
            color=LINK_COLOURS.get(r["links"], GRAY),
            label=f"{r['links']}",
        )
    ratio_ax.axhline(1.2, color=ORANGE, linestyle="--", linewidth=1.0, label="20% more (1.2)")
    ratio_ax.axhline(1.0, color=GRAY, linewidth=0.8)
    ratio_ax.set_xticks(qx)
    ratio_ax.set_xticklabels([label for _, label in quantiles])
    ratio_ax.set_xlabel("over the repaired draws: quantile")
    ratio_ax.set_ylabel("backup delay / delay before the failure")
    ratio_ax.set_title("StarGlider: the ratio's quantiles per link set", fontsize=9)
    ratio_ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.2), ncol=3, fontsize=8)

    kinds = ordered({r["kind"] for r in d.percolation}, ("satellites", "laser-links"))
    styles = dict(zip(kinds, ("-", "--", ":"), strict=False))
    for links in ordered({r["links"] for r in d.percolation}, ("plus-grid", "three")):
        for kind in kinds:
            rs = sorted(
                pick(d.percolation, links=links, kind=kind), key=lambda r: float(r["failed_share"])
            )
            if not rs:
                continue
            colour = LINK_COLOURS.get(links, GRAY)
            xs = values(rs, "failed_share")
            seeds = sorted({r["seeds"] for r in rs})
            perc_ax.plot(
                xs,
                values(rs, "mean_largest_piece_share"),
                color=colour,
                linestyle=styles[kind],
                marker="o",
                markersize=3,
                label=f"{links}, failed {kind} ({'/'.join(seeds)} seeds)",
            )
            perc_ax.fill_between(
                xs,
                values(rs, "min_largest_piece_share"),
                values(rs, "max_largest_piece_share"),
                color=colour,
                alpha=0.12,
                linewidth=0,
            )
            for t in sorted({num(r["threshold_failed_share"]) for r in rs}):
                perc_ax.axvline(t, color=colour, linestyle=styles[kind], linewidth=0.9, alpha=0.7)
    perc_ax.plot([], [], color=GRAY, linewidth=0.9, label="vertical: endless-grid threshold")
    perc_ax.set_xlabel("failed share")
    perc_ax.set_ylabel("largest piece (share of the working satellites)")
    perc_ax.set_ylim(0, 1.03)
    perc_ax.set_title(
        "Percolation on Hypatia's ideal shell: mean, band min to max over the seeds", fontsize=9
    )
    perc_ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.12), ncol=2, fontsize=8)
    fig.suptitle("Comparisons with no pass or fail (spec 6.4), Hypatia's ideal shell")
    save(fig, out, "comparisons.png")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Draw the Phase 4 report's plots.")
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)
    needed = ("runs.csv", "scenarios.csv", "measures.csv", "recovery.csv", "recovery_summary.csv")
    missing = [n for n in needed if not (args.data / n).is_file()]
    missing += [
        n
        for n in ("starglider.csv", "percolation.csv")
        if not (args.data / n).is_file() and not (DEFAULT_DATA / n).is_file()
    ]
    if missing:
        print(
            f"error: no {', '.join(missing)} in {args.data} (run python/run_phase4.py and "
            "python/compare_failures.py)",
            file=sys.stderr,
        )
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    d = Data(args.data)
    plot_lost_time(d, args.out)
    plot_lost_by_offset(d, args.out)
    plot_restore_settle(d, args.out)
    plot_extra_delay(d, args.out)
    plot_detection(d, args.out)
    plot_transient(d, args.out)
    plot_planner_variants(d, args.out)
    plot_compute(d, args.out)
    plot_messages(d, args.out)
    plot_repointing(d, args.out)
    plot_gateway(d, args.out)
    plot_reachability(d, args.out)
    plot_recovery_delay(d, args.out)
    plot_largest_piece(d, args.out)
    plot_comparisons(d, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

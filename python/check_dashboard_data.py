#!/usr/bin/env python3
"""Check the dashboard's exported data against the engine's CSV files (the data check).

docs/phase-5-dashboard.md sections 6.1 and 6.2. Reads the files python/export_dashboard.py
wrote into --data (default dashboard/public/data/) and the programs' CSVs and summaries it kept
under <results>/<mode>/ (default results/dashboard/, the mode from the manifest), and compares
them with code of its own: nothing here is imported from the export or the Phase 3 and 4
runners, so a mistake in their parsing or conversion cannot hide itself. Compared:

  - every manifest entry with the file on disk (size, sha256), no file in --data outside the
    manifest, and no file of 25 MiB or more;
  - every satellite's catalogue number, name, shell, plane and place in the plane with the
    `constellation` CSV (the satellites in planes, in catalogue order), every ground site with
    data/ground/;
  - every position sample with the `propagate` CSV, bit for bit as a 32-bit float (NaN where
    the CSV has no row);
  - every laser link of every pattern with the `--links-out` file;
  - the per-shell table and the link counts with the `constellation` and `topology`
    summaries;
  - the route story: every delay, every interval of one route (rebuilt here from the routes
    CSV), every drop with the drops CSV, every summary number with the `routing` summary;
  - the failure story: every interval of every lane with the events CSV, every plan with the
    routes CSV, every fact with the facts CSV, the first flood instant of every satellite and
    site (worked out here from the floods CSV), every measure with the measures CSV and with
    docs/phase-4-data/measures.csv, the failed elements with the failure file, the slow-motion
    spans, and the context rows with docs/phase-4-data/measures.csv;
  - the falling-apart story: every chart cell with docs/phase-4-data/, every failed
    satellite with the failure file, every failed link with the facts CSV, the planned links
    that did not fail with links-three.csv and the facts CSV, every route with the routes CSV,
    every network row with the network CSV;
  - that every story's time range (its run, or its one step) lies inside the position window;
  - the engine in the page (engine/): the WebAssembly program byte for byte with the build in
    --engine (default build/wasm/bin), every input byte for byte with the snapshot's and the
    ground data's own file, the manifest's `engine` block (its file lists, the settings against
    the route story's, the on-demand strategies, its window against the position window, and
    every flag of its argument list against those settings). That the program reproduces the
    route story is dashboard/scripts/check-engine.ts's job, not this one's.

Whole numbers and names must be equal; decimals must equal the number the program wrote
(Python's float() of its text, which a JSON number round-trips exactly). Each difference is
printed with the file, the row and both values.

Then the interpolation of spec 4.3, done here from the exported 32-bit samples, against the
`propagate` CSV at 1 s for every satellite over the error window: the median and the largest
error are printed.

Exit status 1 on any difference, any file of 25 MiB or more, or a largest interpolation error
over 1 km; 2 on a usage error.

Usage (inside the CI image):

    python3 python/check_dashboard_data.py [--data dashboard/public/data] \\
        [--results results/dashboard] [--engine build/wasm/bin]
"""

from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import json
import math
import re
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
GROUND = REPO / "data/ground/2026-11-19"
CITIES = REPO / "data/ground/cities.json"
PHASE4_DATA = REPO / "docs/phase-4-data"
LIMIT_BYTES = 25 * 1024 * 1024
LIMIT_ERROR_KM = 1.0
EARTH_RATE_RAD_S = 7.292115e-5  # Spec 4.3.
PATTERNS = ("plus-grid", "three", "six")
ENGINE_PROGRAM = ("satsim-routing.js", "satsim-routing.wasm")
ENGINE_SNAPSHOT = ("gp.json", "supgp.json", "finals2000A.all")
ENGINE_GROUND = (
    "gateways.json",
    "assumed_gateways.json",
    "pop_codes.json",
    "starlink_pops.csv",
    "starlink_geofeed.csv",
)
ENGINE_STRATEGIES = ["lowest-delay", "keep-until-broken", "threshold:2ms"]


class Check:
    """Collects differences."""

    def __init__(self) -> None:
        self.differences: list[str] = []

    def differ(self, file: str, row, what: str, exported, engine) -> None:
        self.differences.append(f"{file}, {row}: {what}: exported {exported!r}, engine {engine!r}")

    def same(self, file: str, row, what: str, exported, engine) -> bool:
        if exported != engine or type(exported) is not type(engine):
            # An int and a float of the same value are the same number in JSON.
            numbers = (int, float)
            if not (
                isinstance(exported, numbers)
                and isinstance(engine, numbers)
                and not isinstance(exported, bool)
                and not isinstance(engine, bool)
                and exported == engine
            ):
                self.differ(file, row, what, exported, engine)
                return False
        return True

    def decimal(self, file: str, row, what: str, exported, text: str) -> bool:
        """A decimal against the program's text; none, never and empty mean null."""
        engine = None if text in ("", "none", "never") else float(text)
        if engine is None or exported is None or isinstance(exported, bool):
            return self.same(file, row, what, exported, engine)
        if not isinstance(exported, (int, float)) or float(exported) != engine:
            self.differ(file, row, what, exported, text)
            return False
        return True

    def whole(self, file: str, row, what: str, exported, text: str) -> bool:
        engine = None if text in ("", "none", "never") else int(text)
        return self.same(file, row, what, exported, engine)


def rows_of(path: Path) -> list[dict]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def instant(text: str) -> datetime.datetime:
    return datetime.datetime.fromisoformat(text).replace(tzinfo=datetime.UTC)


def after(text: str, start: str) -> float:
    return (instant(text) - instant(start)).total_seconds()


class Names:
    """Exported indices back to the engine's names, refusing indices out of range."""

    def __init__(self, check: Check, satellites: list[int], sites: list[dict]):
        self.check = check
        self.satellites = satellites
        self.sites = sites

    def _in_range(self, i, n: int, where: str) -> bool:
        if not isinstance(i, int) or isinstance(i, bool) or not 0 <= i < n:
            self.check.differ(where, "-", "index out of range", i, f"0 .. {n - 1}")
            return False
        return True

    def sat(self, i, where: str) -> str:
        return str(self.satellites[i]) if self._in_range(i, len(self.satellites), where) else "?"

    def route(self, indices, where: str) -> str:
        if not isinstance(indices, list):
            self.check.differ(where, "-", "route is not a list", indices, "a list")
            return "?"
        return " ".join(self.sat(i, where) for i in indices)

    def site(self, i, kind: str, where: str) -> str:
        if i is None:
            return ""
        if not self._in_range(i, len(self.sites), where):
            return "?"
        s = self.sites[i]
        if s["kind"] != kind:
            self.check.differ(where, "-", f"site {i} kind", s["kind"], kind)
        return s["name"]

    def node(self, value, where: str) -> str:
        """[kind, index] -> satellite:<n>, city:<name> or gateway:<name>."""
        if not isinstance(value, list) or len(value) < 2:
            return "?"
        kind, i = value[0], value[1]
        if kind == "satellite":
            return f"satellite:{self.sat(i, where)}"
        return f"{kind}:{self.site(i, kind, where)}"

    def element(self, value, where: str) -> str:
        """A failed element array -> satellite:<n>, link:<a>-<b>, gateway:<name>; null -> ""."""
        if value is None:
            return ""
        if isinstance(value, list) and value and value[0] == "link" and len(value) == 3:
            return f"link:{self.sat(value[1], where)}-{self.sat(value[2], where)}"
        return self.node(value, where)


# ---------------------------------------------------------------------------------------
def check_files(check: Check, data: Path, manifest: dict) -> None:
    listed = set()
    for entry in manifest["files"]:
        path = data / entry["path"]
        listed.add(entry["path"])
        if not path.is_file():
            check.differ("manifest.json", entry["path"], "file", "listed", "missing on disk")
            continue
        content = path.read_bytes()
        check.same("manifest.json", entry["path"], "bytes", entry["bytes"], len(content))
        check.same(
            "manifest.json",
            entry["path"],
            "sha256",
            entry["sha256"],
            hashlib.sha256(content).hexdigest(),
        )
    for path in sorted(p for p in data.rglob("*") if p.is_file()):
        name = path.relative_to(data).as_posix()
        if path.stat().st_size >= LIMIT_BYTES:
            check.differ(name, "-", "size", path.stat().st_size, f"under {LIMIT_BYTES}")
        if name != "manifest.json" and name not in listed:
            check.differ(name, "-", "file", "on disk", "not in the manifest")


def same_bytes(check: Check, data: Path, exported: str, original: Path) -> None:
    if not original.is_file():
        check.differ(exported, "-", "original", "exported", f"{original} missing")
    elif not (data / exported).is_file():
        check.differ(exported, "-", "file", "listed", "missing on disk")
    elif (data / exported).read_bytes() != original.read_bytes():
        check.differ(exported, "-", "content", "exported", f"differs from {original}")


def check_engine(check: Check, data: Path, manifest: dict, story: dict, engine: Path) -> None:
    """The engine/ files and the manifest's engine block (see the module docstring)."""
    file = "manifest.json"
    block = manifest.get("engine")
    if not isinstance(block, dict):
        check.differ(file, "engine", "block", block, "an object")
        return
    program = {"js": f"engine/{ENGINE_PROGRAM[0]}", "wasm": f"engine/{ENGINE_PROGRAM[1]}"}
    check.same(file, "engine", "program", block.get("program"), program)
    snapshot = [f"engine/snapshot/{n}" for n in ENGINE_SNAPSHOT]
    ground = [f"engine/ground/{n}" for n in ENGINE_GROUND]
    check.same(file, "engine", "snapshot", block.get("snapshot"), snapshot)
    check.same(file, "engine", "ground", block.get("ground"), ground)
    for name in ENGINE_PROGRAM:
        same_bytes(check, data, f"engine/{name}", engine / name)
    for name in ENGINE_SNAPSHOT:
        original = REPO / "data/starlink" / manifest["snapshot"] / name
        same_bytes(check, data, f"engine/snapshot/{name}", original)
    for name in ENGINE_GROUND:
        same_bytes(
            check, data, f"engine/ground/{name}", REPO / "data/ground" / manifest["ground"] / name
        )
    listed = {e["path"] for e in manifest["files"]}
    for path in [*program.values(), *snapshot, *ground]:
        if path not in listed:
            check.differ(file, "engine", "file", path, "not in files")

    s, story_settings = block.get("settings", {}), story["settings"]
    for key in ("model", "pattern", "replan_s", "planner", "backup", "step_s"):
        check.same(file, "engine.settings", key, s.get(key), story_settings.get(key))
    check.same(
        file,
        "engine.settings",
        "plan_time_utc",
        s.get("plan_time_utc"),
        manifest["settings"]["plan_time_utc"],
    )
    check.same(file, "engine.settings", "roles", s.get("roles"), "study")
    check.same(file, "engine", "strategies", block.get("strategies"), ENGINE_STRATEGIES)
    window = {
        "start_utc": manifest["settings"]["window_start_utc"],
        "duration_s": manifest["settings"]["window_s"],
    }
    check.same(file, "engine", "window", block.get("window"), window)

    arguments = block.get("arguments")
    if (
        not isinstance(arguments, list)
        or len(arguments) % 2
        or not all(isinstance(a, str) for a in arguments)
    ):
        check.differ(file, "engine", "arguments", arguments, "flag and value pairs")
        return
    given = dict(zip(arguments[::2], arguments[1::2], strict=True))
    check.same(file, "engine.arguments", "flags", len(given), len(arguments) // 2)
    expected = {
        "--snapshot": "{snapshot}",
        "--ground": "{ground}",
        "--cities": "{cities}",
        "--start": "{start_utc}",
        "--duration-s": "{duration_s}",
        "--step-s": str(s.get("step_s")),
        "--pattern": s.get("pattern"),
        "--model": s.get("model"),
        "--roles": "study",
        "--strategies": "{strategies}",
        "--replan-s": str(s.get("replan_s")),
        "--planner": s.get("planner"),
        "--backup": s.get("backup"),
        "--out": "-",
        "--drops-out": "{drops}",
        "--plan-time": s.get("plan_time_utc"),
    }
    check.same(file, "engine.arguments", "flag names", sorted(given), sorted(expected))
    for flag, value in expected.items():
        check.same(file, "engine.arguments", flag, given.get(flag), value)


def check_satellites(check: Check, satellites: dict, res: Path) -> list[int]:
    engine = sorted(
        (r for r in rows_of(res / "constellation.csv") if r["plane_index"] != ""),
        key=lambda r: int(r["norad_cat_id"]),
    )
    n = len(engine)
    for key in ("norad_cat_id", "name", "shell", "plane", "index_in_plane"):
        check.same("satellites.json", "-", f"length of {key}", len(satellites[key]), n)
    shells = satellites["shells"]
    for i, r in enumerate(engine[: len(satellites["norad_cat_id"])]):
        row = f"satellite {i}"
        check.whole(
            "satellites.json", row, "norad_cat_id", satellites["norad_cat_id"][i], r["norad_cat_id"]
        )
        check.same("satellites.json", row, "name", satellites["name"][i], r["object_name"])
        k = satellites["shell"][i]
        name = shells[k] if isinstance(k, int) and 0 <= k < len(shells) else k
        check.same("satellites.json", row, "shell", name, r["shell"])
        check.whole("satellites.json", row, "plane", satellites["plane"][i], r["plane_index"])
        check.whole(
            "satellites.json",
            row,
            "index_in_plane",
            satellites["index_in_plane"][i],
            r["index_in_plane"],
        )
    return [int(r["norad_cat_id"]) for r in engine]


def check_sites(check: Check, sites: list[dict]) -> None:
    cities = json.loads(CITIES.read_text(encoding="utf-8"))
    names = {n for p in cities["pairs"] if p["role"] == "study" for n in (p["from"], p["to"])}
    expected = [
        {
            "name": c["name"],
            "kind": "city",
            "lat_deg": c["lat"],
            "lon_deg": c["lon"],
            "country": c["country"],
        }
        for c in cities["cities"]
        if c["name"] in names
    ]
    for file, source in (("gateways.json", "real"), ("assumed_gateways.json", "assumed")):
        for g in json.loads((GROUND / file).read_text(encoding="utf-8")):
            expected.append(
                {
                    "name": g["name"],
                    "kind": "gateway",
                    "lat_deg": g["lat"],
                    "lon_deg": g["lon"],
                    "status": g["status"],
                    "source": source,
                }
            )
    for p in json.loads((GROUND / "pop_codes.json").read_text(encoding="utf-8")):
        expected.append(
            {
                "name": p["code"],
                "kind": "pop",
                "lat_deg": p["lat"],
                "lon_deg": p["lon"],
                "city": p["city"],
                "country": p["country"],
            }
        )
    check.same("sites.json", "-", "sites", len(sites), len(expected))
    for i, (mine, theirs) in enumerate(zip(sites, expected, strict=False)):
        check.same("sites.json", f"site {i}", "fields", sorted(mine), sorted(theirs))
        for k, v in theirs.items():
            check.same("sites.json", f"site {i} ({theirs['name']})", k, mine.get(k), v)


def read_samples(data: Path, manifest: dict, satellites: int) -> np.ndarray | None:
    settings = manifest["settings"]
    samples = settings["window_s"] // settings["sample_step_s"] + 1
    if settings["samples"] != samples:
        return None
    chunks, expected_first = [], 0
    for entry in manifest["positions"]:
        if entry["first_sample"] != expected_first:
            return None
        raw = (data / entry["path"]).read_bytes()
        if len(raw) != entry["samples"] * satellites * 12:
            return None
        chunks.append(np.frombuffer(raw, dtype="<f4").reshape(entry["samples"], satellites, 3))
        expected_first += entry["samples"]
    if expected_first != samples:
        return None
    return np.concatenate(chunks)


def engine_positions(path: Path, start: str, step_s: int, steps: int, norads: list[int]):
    """(steps, satellites, 3) ECEF km in float64 from a propagate CSV, and whether each
    (step, satellite) has a row."""
    column = {n: i for i, n in enumerate(norads)}
    xyz = np.zeros((steps, len(norads), 3))
    seen = np.zeros((steps, len(norads)), dtype=bool)
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        header = next(reader)
        u, n = header.index("utc"), header.index("norad_cat_id")
        x, y, z = (header.index(c) for c in ("ecef_x_km", "ecef_y_km", "ecef_z_km"))
        cache: dict[str, int] = {}
        for line in reader:
            k = cache.get(line[u])
            if k is None:
                t = after(line[u], start)
                k = cache[line[u]] = int(t // step_s)
                if k * step_s != t or not 0 <= k < steps:
                    raise SystemExit(f"error: {path}: {line[u]} is not one of its steps")
            j = column[int(line[n])]
            xyz[k, j] = (float(line[x]), float(line[y]), float(line[z]))
            seen[k, j] = True
    return xyz, seen


def check_positions(check: Check, samples, res: Path, manifest: dict, norads: list[int]) -> None:
    settings = manifest["settings"]
    if samples is None:
        check.differ(
            "manifest.json",
            "positions",
            "samples",
            manifest["positions"],
            "files covering every sample once, of the satellites' size",
        )
        return
    xyz, seen = engine_positions(
        res / "positions.csv",
        settings["window_start_utc"],
        settings["sample_step_s"],
        samples.shape[0],
        norads,
    )
    expected = np.where(seen[..., None], xyz.astype(np.float32), np.float32(np.nan))
    bits_mine = samples.view(np.uint32)
    bits_theirs = expected.astype("<f4").view(np.uint32)
    nan_mine, nan_theirs = np.isnan(samples), np.isnan(expected)
    wrong = (bits_mine != bits_theirs) & ~(nan_mine & nan_theirs)
    for k, j, a in zip(*np.nonzero(wrong), strict=True):
        check.differ(
            "positions",
            f"sample {k}, satellite {j} ({norads[j]})",
            "xyz"[a],
            float(samples[k, j, a]),
            float(xyz[k, j, a]) if seen[k, j] else "no row",
        )


def rotate_z(v: np.ndarray, angle_rad: float) -> np.ndarray:
    c, s = math.cos(angle_rad), math.sin(angle_rad)
    out = v.copy()
    out[..., 0] = c * v[..., 0] - s * v[..., 1]
    out[..., 1] = s * v[..., 0] + c * v[..., 1]
    return out


def interpolate(p0: np.ndarray, p1: np.ndarray, dt_s: float, share: float) -> np.ndarray:
    """Spec 4.3 for many satellites at once: p0, p1 (n, 3) ECEF km at t0 and t0 + dt_s; the
    position at t0 + share x dt_s."""
    later = rotate_z(p1, EARTH_RATE_RAD_S * dt_s)  # Into t0's Earth-fixed axes.
    r0, r1 = np.linalg.norm(p0, axis=1), np.linalg.norm(later, axis=1)
    u0, u1 = p0 / r0[:, None], later / r1[:, None]
    angle = np.arctan2(np.linalg.norm(np.cross(u0, u1), axis=1), np.sum(u0 * u1, axis=1))
    sin_angle = np.sin(angle)
    small = sin_angle < 1e-12
    safe = np.where(small, 1.0, sin_angle)
    a = np.where(small, 1.0 - share, np.sin((1.0 - share) * angle) / safe)
    b = np.where(small, share, np.sin(share * angle) / safe)
    direction = a[:, None] * u0 + b[:, None] * u1
    direction /= np.linalg.norm(direction, axis=1)[:, None]
    mixed = direction * ((1.0 - share) * r0 + share * r1)[:, None]
    return rotate_z(mixed, -EARTH_RATE_RAD_S * share * dt_s)


def interpolation_error(samples, res: Path, manifest: dict, norads: list[int]) -> np.ndarray:
    settings = manifest["settings"]
    step, sample_step = settings["error_step_s"], settings["sample_step_s"]
    steps = settings["error_window_s"] // step + 1
    xyz, seen = engine_positions(
        res / "positions-1s.csv", settings["window_start_utc"], step, steps, norads
    )
    data = samples.astype(np.float64)
    errors = []
    for k in range(steps):
        t = k * step
        s0, offset = divmod(t, sample_step)
        if offset == 0:
            at = data[s0]
        else:
            at = interpolate(data[s0], data[s0 + 1], sample_step, offset / sample_step)
        e = np.linalg.norm(at - xyz[k], axis=1)
        errors.append(e[seen[k] & ~np.isnan(e)])
    return np.concatenate(errors)


# ---------------------------------------------------------------------------------------
def link_rows(path: Path) -> list[tuple[str, str]]:
    found, header = [], None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line:
            continue
        if header is None:
            header = line
            continue
        a, b = line.split(",")[:2]
        found.append((a, b))
    return found


def check_links(check: Check, data: Path, res: Path, names: Names) -> dict[str, int]:
    counts = {}
    for pattern in PATTERNS:
        file = f"links-{pattern}.json"
        exported = json.loads((data / file).read_text(encoding="utf-8"))
        check.same(file, "-", "pattern", exported["pattern"], pattern)
        engine = link_rows(res / f"links-{pattern}.csv")
        check.same(file, "-", "links", len(exported["links"]), len(engine))
        for i, (mine, theirs) in enumerate(zip(exported["links"], engine, strict=False)):
            pair = (names.sat(mine[0], file), names.sat(mine[1], file))
            check.same(file, f"link {i}", "satellites", pair, theirs)
        counts[pattern] = len(exported["links"])
    return counts


SHELL_LINE = re.compile(
    r"^  shell (?P<shell>\S+) \(inclination (?P<inc>\S+) \+- (?P<tol>\S+) deg, height "
    r"(?P<hmin>\S+) to (?P<hmax>\S+) km, plane gap (?P<gap>\S+) deg, planes of (?P<minsize>\d+) "
    r"or more\): (?P<bands>\d+) satellites in the bands, (?P<inplanes>\d+) in planes, "
    r"(?P<planes>\d+) planes, plane size min (?P<smin>\d+) median (?P<smed>\S+) max (?P<smax>\d+)"
)


def check_constellation(
    check: Check, story: dict, res: Path, link_counts: dict, shells: list[str]
) -> None:
    file = "story-constellation.json"
    text = (res / "constellation.summary.txt").read_text(encoding="utf-8")
    engine = [m.groupdict() for m in map(SHELL_LINE.match, text.splitlines()) if m]
    check.same(
        file, "-", "shells", [s["shell"] for s in story["shells"]], [e["shell"] for e in engine]
    )
    check.same("satellites.json", "-", "shells", shells, [e["shell"] for e in engine])
    fields = {
        "inclination_deg": "inc",
        "inclination_tolerance_deg": "tol",
        "height_min_km": "hmin",
        "height_max_km": "hmax",
        "plane_gap_deg": "gap",
        "plane_size_median": "smed",
    }
    counts = {
        "min_plane_size": "minsize",
        "satellites_in_bands": "bands",
        "satellites_in_planes": "inplanes",
        "planes": "planes",
        "plane_size_min": "smin",
        "plane_size_max": "smax",
    }
    for mine, theirs in zip(story["shells"], engine, strict=False):
        for k, g in fields.items():
            check.decimal(file, f"shell {theirs['shell']}", k, mine[k], theirs[g])
        for k, g in counts.items():
            check.whole(file, f"shell {theirs['shell']}", k, mine[k], theirs[g])
    check.same(file, "-", "patterns", [p["pattern"] for p in story["patterns"]], list(PATTERNS))
    for p in story["patterns"]:
        text = (res / f"topology-{p['pattern']}.summary.txt").read_text(encoding="utf-8")
        lines = text.splitlines()
        at = next(i for i, line in enumerate(lines) if line.startswith("  planned laser links: "))
        check.whole(file, p["pattern"], "links", p["links"], lines[at].rsplit(" ", 1)[1])
        check.same(
            file, p["pattern"], "links in the links file", p["links"], link_counts[p["pattern"]]
        )
        kinds = {}
        for line in lines[at + 1 :]:
            m = re.fullmatch(r"    ([a-z_]+): (\d+)", line)
            if not m:
                break
            kinds[m.group(1)] = int(m.group(2))
        check.same(file, p["pattern"], "by_kind", p["by_kind"], kinds)
        check.same(file, p["pattern"], "file", p["file"], f"links-{p['pattern']}.json")


# ---------------------------------------------------------------------------------------
def routing_summary(text: str) -> dict:
    """(from, to, strategy) -> the summary's numbers, read here with patterns of our own."""
    found, direction, lane = {}, None, None
    for line in text.splitlines():
        if m := re.fullmatch(r"    (\S.*) -> (.*) \(\w+, \d+ km, lower bound \S+ ms\)", line):
            direction = (m.group(1), m.group(2))
        elif direction and (m := re.fullmatch(r"      (\S+): ok (\d+) of (\d+) steps; .*", line)):
            lane = found[(*direction, m.group(1))] = {"ok": m.group(2), "steps": m.group(3)}
        elif lane is not None and line.startswith("        path changes per hour: "):
            m = re.search(
                r"counts: bad (\d+), increases (\d+), breaks (\d+), recoveries (\d+)", line
            )
            lane.update(zip(("bad", "increases", "breaks", "recoveries"), m.groups(), strict=True))
        elif lane is not None and line.startswith("        delay: "):
            m = re.search(r"median (\S+) ms", line)
            lane["median"] = m.group(1) if m else ""
        elif lane is not None and line.startswith("        no route: "):
            causes, events = line.removeprefix("        no route: ").split("; drop events ")
            lane["drop_events"] = events
            lane["causes"] = dict(re.findall(r"([a-z_]+) ([0-9.]+) s", causes))
    return found


def check_route(
    check: Check, story: dict, res: Path, names: Names, window_start: str, window_s: int
) -> None:
    file = "story-route.json"
    settings = story["settings"]
    start, duration = settings["start_utc"], settings["duration_s"]
    check.same(file, "-", "run_start_s", story["run_start_s"], after(start, window_start))
    inside(check, file, "-", story["run_start_s"], duration, window_s)
    routes = rows_of(res / story["sources"]["routes"])
    drops = rows_of(res / story["sources"]["drops"])
    summary = routing_summary((res / story["sources"]["summary"]).read_text(encoding="utf-8"))
    by_lane: dict[tuple, list[dict]] = {}
    for r in routes:
        by_lane.setdefault((r["from"], r["to"], r["strategy"]), []).append(r)
    seen = set()
    for d in story["directions"]:
        a, b = names.site(d["from"], "city", file), names.site(d["to"], "city", file)
        for strategy, lane in d["strategies"].items():
            where = f"{a} -> {b} {strategy}"
            seen.add((a, b, strategy))
            rows = sorted(by_lane.get((a, b, strategy), []), key=lambda r: after(r["utc"], start))
            check.same(file, where, "steps", len(lane["delay_ms"]), len(rows))
            check.same(file, where, "steps of the run", len(rows), duration + 1)
            expected_intervals = []
            for k, r in enumerate(rows):
                t = after(r["utc"], start)
                check.same(file, where, f"time of step {k}", float(k), t)
                if k < len(lane["delay_ms"]):
                    text = r["one_way_ms"] if r["status"] == "ok" else ""
                    check.decimal(file, f"{where} step {k}", "delay_ms", lane["delay_ms"][k], text)
                if r["status"] != "ok":
                    continue
                key = (r["route"], r["gateway"])
                last = expected_intervals[-1] if expected_intervals else None
                if last and last[1] == t - 1 and last[2] == key:
                    last[1] = t
                else:
                    expected_intervals.append([t, t, key])
            mine = [
                [x[0], x[1], (names.route(x[2], file), names.site(x[3], "gateway", file))]
                for x in lane["intervals"]
            ]
            check.same(file, where, "intervals", len(mine), len(expected_intervals))
            for i, (m, e) in enumerate(zip(mine, expected_intervals, strict=False)):
                check.same(
                    file,
                    f"{where} interval {i}",
                    "first, last, route",
                    m,
                    [int(e[0]), int(e[1]), e[2]],
                )
            engine_drops = [
                x for x in drops if (x["from"], x["to"], x["strategy"]) == (a, b, strategy)
            ]
            check.same(file, where, "drops", len(lane["drops"]), len(engine_drops))
            for i, (m, e) in enumerate(zip(lane["drops"], engine_drops, strict=False)):
                row = f"{where} drop {i}"
                check.same(file, row, "start_s", m[0], after(e["start_utc"], start))
                check.decimal(file, row, "length_s", m[1], e["length_s"])
                check.same(file, row, "cause", m[2], e["cause"])
                element = m[3]
                if e["satellite_norad_cat_id"]:
                    expected = ["satellite", e["satellite_norad_cat_id"]]
                    got = [element[0], names.sat(element[1], file)] if element else None
                elif e["link_norad_cat_id_a"]:
                    expected = [
                        "link",
                        e["link_norad_cat_id_a"],
                        e["link_norad_cat_id_b"],
                        e["link_down_reason"],
                    ]
                    got = (
                        [
                            element[0],
                            names.sat(element[1], file),
                            names.sat(element[2], file),
                            element[3],
                        ]
                        if element
                        else None
                    )
                elif e["site"]:
                    expected = [e["site_kind"], e["site"]]
                    got = (
                        [element[0], names.site(element[1], element[0], file)] if element else None
                    )
                else:
                    expected, got = None, element
                check.same(file, row, "element", got, expected)
            s, engine = lane["summary"], summary.get((a, b, strategy))
            if engine is None:
                check.differ(file, where, "summary", "present", "not in the routing summary")
                continue
            check.whole(file, where, "ok_steps", s["ok_steps"], engine["ok"])
            check.whole(file, where, "steps", s["steps"], engine["steps"])
            check.decimal(file, where, "delay_median_ms", s["delay_median_ms"], engine["median"])
            check.whole(file, where, "bad_changes", s["bad_changes"], engine["bad"])
            check.whole(file, where, "increases", s["increases"], engine["increases"])
            check.same(
                file, where, "changes", s["changes"], int(engine["bad"]) + int(engine["increases"])
            )
            check.whole(file, where, "breaks", s["breaks"], engine["breaks"])
            check.whole(file, where, "recoveries", s["recoveries"], engine["recoveries"])
            check.whole(file, where, "drop_events", s["drop_events"], engine["drop_events"])
            check.same(
                file,
                where,
                "no_route_s",
                s["no_route_s"],
                {k: float(v) for k, v in engine["causes"].items()},
            )
            check.same(
                file,
                where,
                "no_route_total_s",
                s["no_route_total_s"],
                sum(float(v) for v in engine["causes"].values()),
            )
    check.same(file, "-", "lanes", sorted(seen), sorted(by_lane))


# ---------------------------------------------------------------------------------------
MEASURES = (
    "failure_s",
    "black_hole_s",
    "loop_s",
    "lost_s",
    "restore_s",
    "settle_s",
    "extra_min_ms",
    "extra_median_ms",
    "extra_max_ms",
    "extra_settled_ms",
)


def failure_file(path: Path) -> list[list[str]]:
    lines = [x for x in path.read_text(encoding="utf-8").splitlines() if not x.startswith("#")]
    return [r for r in csv.reader(lines[1:])]


def check_failed(
    check: Check, file: str, where: str, failed: dict, path: Path, names: Names
) -> None:
    rows = failure_file(path)
    sats = [r[1] for r in rows if r[0] == "satellite"]
    links = [(r[1], r[2]) for r in rows if r[0] == "laser_link"]
    gateways = [r[3] for r in rows if r[0] == "gateway"]
    check.same(
        file, where, "failed satellites", [names.sat(i, file) for i in failed["satellites"]], sats
    )
    check.same(
        file,
        where,
        "failed links",
        [(names.sat(a, file), names.sat(b, file)) for a, b in failed["links"]],
        links,
    )
    check.same(
        file,
        where,
        "failed gateways",
        [names.site(i, "gateway", file) for i in failed["gateways"]],
        gateways,
    )


def inside(check: Check, file: str, where: str, start_s, duration_s, window_s: int) -> None:
    """A story's time range, in seconds from the window start, lies inside the window."""
    if not (start_s >= 0 and start_s + duration_s <= window_s):
        check.differ(
            file,
            where,
            "time range",
            [start_s, start_s + duration_s],
            f"inside the position window [0, {window_s}]",
        )


def check_failure(
    check: Check,
    story: dict,
    res: Path,
    names: Names,
    window_start: str,
    window_s: int,
    sites: int,
) -> None:
    file = "story-failure.json"
    a, b = names.site(story["from"], "city", file), names.site(story["to"], "city", file)
    committed = rows_of(PHASE4_DATA / "measures.csv")
    for sc in story["scenarios"]:
        src = {k: res / v for k, v in sc["sources"].items()}
        where = sc["scenario"]
        check.same(
            file, where, "run_start_s", sc["run_start_s"], after(sc["start_utc"], window_start)
        )
        events = rows_of(src["events"])
        lanes: dict[str, list[dict]] = {}
        for e in events:
            check.same(file, where, "event direction", (e["from"], e["to"]), (a, b))
            lanes.setdefault(e["reaction"], []).append(e)
        check.same(file, where, "lanes", sorted(sc["lanes"]), sorted(lanes))
        for lane, intervals in sc["lanes"].items():
            engine = lanes.get(lane, [])
            check.same(file, f"{where} {lane}", "intervals", len(intervals), len(engine))
            for i, (m, e) in enumerate(zip(intervals, engine, strict=False)):
                row = f"{where} {lane} interval {i}"
                check.decimal(file, row, "start_s", m[0], e["start_s"])
                check.decimal(file, row, "end_s", m[1], e["end_s"])
                check.same(file, row, "state", m[2], e["state"])
                check.same(file, row, "drop_cause", m[3] or "", e["drop_cause"])
                check.same(
                    file, row, "failed_element", names.element(m[4], file), e["failed_element"]
                )
                check.decimal(file, row, "one_way_ms", m[5], e["one_way_ms"])
                check.same(file, row, "route", names.route(m[6], file), e["route"])
                check.same(
                    file,
                    row,
                    "serving",
                    names.sat(m[7], file) if m[7] is not None else "",
                    e["serving_norad_cat_id"],
                )
                check.same(file, row, "gateway", names.site(m[8], "gateway", file), e["gateway"])
            if engine:
                check.decimal(
                    file, where, f"duration_s ({lane})", sc["duration_s"], engine[-1]["end_s"]
                )
        routes = rows_of(src["routes"])
        planned = sorted({r["reaction"] for r in routes if r["plan"]})
        check.same(file, where, "lanes with plans", sorted(sc["plans"]), planned)
        for lane, plans in sc["plans"].items():
            engine = [
                [after(r["utc"], sc["start_utc"]), r["plan"]]
                for r in routes
                if r["reaction"] == lane and r["plan"]
            ]
            check.same(file, f"{where} {lane}", "plans", plans, engine)
        pops = {r["home_pop"] for r in routes if r["home_pop"]}
        check.same(
            file,
            where,
            "home_pop",
            names.site(sc["home_pop"], "pop", file),
            pops.pop() if len(pops) == 1 else sorted(pops),
        )
        measures = {r["reaction"]: r for r in rows_of(src["measures"])}
        check.same(file, where, "measures reactions", sorted(sc["measures"]), sorted(measures))
        run = sc["context"]["this_run"]
        theirs = {r["reaction"]: r for r in committed if r["run"] == run and r["group"] == "main"}
        for reaction, m in sc["measures"].items():
            e = measures.get(reaction, {})
            p4 = theirs.get(reaction)
            for k in (*MEASURES, "reports", "messages"):
                row = f"{where} {reaction}"
                if k in ("reports", "messages"):
                    check.whole(file, row, k, m[k], e.get(k, "missing"))
                else:
                    check.decimal(file, row, k, m[k], e.get(k, "missing"))
                if p4 is None:
                    check.differ(file, row, "committed row", run, "not in measures.csv")
                elif k in ("reports", "messages"):
                    check.whole(file, f"{row} (docs/phase-4-data/measures.csv)", k, m[k], p4[k])
                else:
                    check.decimal(file, f"{row} (docs/phase-4-data/measures.csv)", k, m[k], p4[k])
        check.decimal(
            file,
            where,
            "failure_s",
            sc["failure_s"],
            measures.get("planner", {}).get("failure_s", ""),
        )
        settles = [
            float(r["settle_s"]) if r["settle_s"] != "none" else None for r in measures.values()
        ]
        t_f = float(measures["planner"]["failure_s"])
        end = float(sc["duration_s"]) if None in settles else t_f + max(settles)
        expected = [{"from_s": t_f, "to_s": t_f + 1.0, "speed": 0.02}]
        if end > t_f + 1.0:
            expected.append({"from_s": t_f + 1.0, "to_s": end, "speed": 2})
        check.same(file, where, "slow motion spans", len(sc["slow_motion"]), len(expected))
        for span, e in zip(sc["slow_motion"], expected, strict=False):
            for k in ("from_s", "to_s", "speed"):
                check.same(file, where, f"slow motion {k}", span.get(k), e[k])
        inside(check, file, where, sc["run_start_s"], sc["duration_s"], window_s)
        facts = rows_of(src["facts"])
        check.same(file, where, "facts", len(sc["facts"]), len(facts))
        for m, e in zip(sc["facts"], facts, strict=False):
            row = f"{where} fact {e['fact']}"
            check.whole(file, row, "fact", m["fact"], e["fact"])
            check.same(file, row, "subject", names.element(m["subject"], file), e["subject"])
            check.same(file, row, "state", m["state"], e["state"])
            check.decimal(file, row, "t_s", m["t_s"], e["t_s"])
            check.same(file, row, "old", m["old"], e["old"] == "yes")
            detectors = e["detectors"].split(";") if e["detectors"] else []
            check.same(file, row, "detectors", len(m["detectors"]), len(detectors))
            for d, text in zip(m["detectors"], detectors, strict=False):
                node, at = text.rsplit("@", 1)
                check.same(file, row, "detector", names.node(d[:2], file), node)
                check.decimal(file, row, "detector instant", d[2], at)
            for k in ("planner_known_s", "flood_last_s"):
                check.decimal(file, row, k, m[k], e[k])
            for k in ("reports", "report_hops", "flood_nodes", "messages"):
                check.whole(file, row, k, m[k], e[k])
        first_sat: dict[str, float] = {}
        first_site: dict[str, float] = {}
        with src["floods"].open(newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                target = first_sat if r["node"].startswith("satellite:") else first_site
                t = float(r["learn_s"])
                if r["node"] not in target or t < target[r["node"]]:
                    target[r["node"]] = t
        flood = sc["flood"]
        check.same(
            file,
            where,
            "flood satellites",
            len(flood["satellite_first_learn_s"]),
            len(names.satellites),
        )
        for i, t in enumerate(flood["satellite_first_learn_s"]):
            node = f"satellite:{names.satellites[i]}" if i < len(names.satellites) else "?"
            check.same(
                file, f"{where} flood satellite {i}", "first learn", t, first_sat.pop(node, None)
            )
        check.same(file, where, "flood sites", len(flood["site_first_learn_s"]), sites)
        for i, t in enumerate(flood["site_first_learn_s"][:sites]):
            s = names.sites[i]
            node = f"{s['kind']}:{s['name']}"
            check.same(
                file, f"{where} flood site {i}", "first learn", t, first_site.pop(node, None)
            )
        for node in (*first_sat, *first_site):
            check.differ(file, where, "flood node", "missing", node)
        check_failed(check, file, where, sc["failed"], src["failures"], names)
        rows = [
            r
            for r in committed
            if r["group"] == "main"
            and r["scenario"] == sc["scenario"]
            and r["variant"] == "default"
        ]
        ctx = sc["context"]
        check.same(file, where, "context source", ctx["source"], "docs/phase-4-data/measures.csv")
        check.same(file, where, "context rows", len(ctx["rows"]), len(rows))
        for i, (m, e) in enumerate(zip(ctx["rows"], rows, strict=False)):
            row = f"{where} context {i}"
            for k in ("run", "model", "from", "to", "reaction"):
                check.same(file, row, k, m[k], e[k])
            check.whole(file, row, "instant", m["instant"], e["instant"])
            check.decimal(file, row, "lost_s", m["lost_s"], e["lost_s"])
        if not any(r["run"] == run for r in rows):
            check.differ(file, where, "this_run", run, "not among the context rows")
        if link_rows(src["links"]) != link_rows(res / "links-three.csv"):
            check.differ(file, where, "laser plan", src["links"].name, "links-three.csv differs")


# ---------------------------------------------------------------------------------------
def check_table(check: Check, file: str, table: dict) -> None:
    path = REPO / table["source"]
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        columns = next(reader)
        rows = list(reader)
    check.same(file, table["source"], "columns", table["columns"], columns)
    check.same(file, table["source"], "rows", len(table["rows"]), len(rows))
    for i, (mine, theirs) in enumerate(zip(table["rows"], rows, strict=False)):
        check.same(file, f"{table['source']} row {i}", "cells", len(mine), len(theirs))
        for column, m, text in zip(columns, mine, theirs, strict=False):
            try:
                float(text)
                numeric = True
            except ValueError:
                numeric = False
            if numeric:
                check.decimal(file, f"{table['source']} row {i}", column, m, text)
            else:
                check.same(
                    file, f"{table['source']} row {i}", column, m, None if text == "" else text
                )


def check_resilience(
    check: Check, story: dict, res: Path, names: Names, window_start: str, window_s: int
):
    file = "story-resilience.json"
    charts = story["charts"]
    check.same(
        file,
        "charts",
        "sources",
        [charts["recovery_summary"]["source"], charts["percolation"]["source"]],
        ["docs/phase-4-data/recovery_summary.csv", "docs/phase-4-data/percolation.csv"],
    )
    for table in charts.values():
        check_table(check, file, table)
    cities = json.loads(CITIES.read_text(encoding="utf-8"))
    pairs = [(p["from"], p["to"]) for p in cities["pairs"] if p["role"] == "study"]
    for step in story["steps"]:
        where = step["name"]
        src = {k: res / v for k, v in step["sources"].items()}
        check.same(file, where, "t_s", step["t_s"], after(step["utc"], window_start))
        inside(check, file, where, step["t_s"], 0, window_s)
        if link_rows(src["links"]) != link_rows(res / "links-three.csv"):
            check.differ(file, where, "laser plan", src["links"].name, "links-three.csv differs")
        check_failed(
            check,
            file,
            where,
            {"satellites": step["failed_satellites"], "links": [], "gateways": []},
            src["failures"],
            names,
        )
        facts = [
            f["subject"]
            for f in rows_of(src["facts"])
            if f["subject"].startswith("link:") and f["state"] == "down"
        ]
        check.same(
            file,
            where,
            "failed links",
            [f"link:{names.sat(x, file)}-{names.sat(y, file)}" for x, y in step["failed_links"]],
            facts,
        )
        down = set(facts)
        planned = link_rows(res / "links-three.csv")
        kept = [
            i
            for i, (x, y) in enumerate(planned)
            if f"link:{min(x, y, key=int)}-{max(x, y, key=int)}" not in down
        ]
        check.same(file, where, "planned_links_not_failed", step["planned_links_not_failed"], kept)
        routes = {(r["from"], r["to"], r["reaction"]): r for r in rows_of(src["routes"])}
        got = [
            (names.site(r["from"], "city", file), names.site(r["to"], "city", file))
            for r in step["routes"]
        ]
        check.same(file, where, "directions", got, pairs)
        for r, (x, y) in zip(step["routes"], got, strict=False):
            for lane in ("baseline", "reference"):
                e = routes.get((x, y, lane))
                row = f"{where} {x} -> {y} {lane}"
                if e is None:
                    check.differ(file, row, "row", "exported", "missing in the routes CSV")
                    continue
                check.same(file, row, "utc", e["utc"], step["utc"])
                m = r[lane]
                check.same(file, row, "state", m["state"], e["state"])
                check.same(file, row, "drop_cause", m["drop_cause"] or "", e["drop_cause"])
                check.decimal(file, row, "one_way_ms", m["one_way_ms"], e["one_way_ms"])
                check.same(file, row, "route", names.route(m["route"], file), e["route"])
        network = rows_of(src["network"])
        check.same(file, where, "network rows", len(step["network"]), len(network))
        for m, e in zip(step["network"], network, strict=False):
            row = f"{where} network {e['scope']}"
            check.same(file, row, "utc", e["utc"], step["utc"])
            check.same(file, row, "scope", m["scope"], e["scope"])
            for k in ("working_satellites", "pieces", "largest_piece"):
                check.whole(file, row, k, m[k], e[k])
            check.decimal(
                file, row, "largest_piece_share", m["largest_piece_share"], e["largest_piece_share"]
            )


# ---------------------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Check the dashboard's exported data.")
    parser.add_argument("--data", type=Path, default=REPO / "dashboard/public/data")
    parser.add_argument("--results", type=Path, default=REPO / "results/dashboard")
    parser.add_argument("--engine", type=Path, default=REPO / "build/wasm/bin")
    args = parser.parse_args(argv)
    manifest_path = args.data / "manifest.json"
    if not manifest_path.is_file():
        print(f"error: {manifest_path} not found", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    res = args.results / manifest["settings"]["mode"]
    if not res.is_dir():
        print(f"error: {res} not found", file=sys.stderr)
        return 2

    def load(name: str):
        return json.loads((args.data / name).read_text(encoding="utf-8"))

    check = Check()
    check_files(check, args.data, manifest)
    satellites = load("satellites.json")
    norads = check_satellites(check, satellites, res)
    check.same("manifest.json", "-", "satellites", manifest["satellites"], len(norads))
    sites = load("sites.json")["sites"]
    check_sites(check, sites)
    names = Names(check, satellites["norad_cat_id"], sites)
    samples = read_samples(args.data, manifest, len(norads))
    print("positions", flush=True)
    check_positions(check, samples, res, manifest, norads)
    link_counts = check_links(check, args.data, res, names)
    check_constellation(
        check, load("story-constellation.json"), res, link_counts, satellites["shells"]
    )
    print("stories", flush=True)
    window_start = manifest["settings"]["window_start_utc"]
    window_s = manifest["settings"]["window_s"]
    if manifest["settings"]["error_window_s"] > window_s:
        check.differ(
            "manifest.json",
            "settings",
            "error_window_s",
            manifest["settings"]["error_window_s"],
            f"at most {window_s}",
        )
    check_route(check, load("story-route.json"), res, names, window_start, window_s)
    check_engine(check, args.data, manifest, load("story-route.json"), args.engine)
    check_failure(check, load("story-failure.json"), res, names, window_start, window_s, len(sites))
    check_resilience(check, load("story-resilience.json"), res, names, window_start, window_s)

    status = 0
    if samples is not None:
        print("interpolation error", flush=True)
        errors = interpolation_error(samples, res, manifest, norads)
        largest = float(errors.max())
        print(
            f"interpolation error over {errors.size:,} positions (1 s, "
            f"{manifest['settings']['error_window_s']} s): median "
            f"{float(np.median(errors)) * 1000:.1f} m, largest {largest * 1000:.1f} m"
        )
        if largest > LIMIT_ERROR_KM:
            print(
                f"FAIL: largest interpolation error {largest:.3f} km is over {LIMIT_ERROR_KM} km",
                file=sys.stderr,
            )
            status = 1
    for d in check.differences:
        print(f"difference: {d}")
    print(f"{len(check.differences)} differences")
    if check.differences:
        status = 1
    print("data check: " + ("PASSED" if status == 0 else "FAILED"))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

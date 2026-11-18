#!/usr/bin/env python3
"""Verify the propagate program against Skyfield on real Starlink data.

Runs the C++ `propagate` program on a fixed sample of satellites and times from a snapshot,
then recomputes every row independently with Skyfield from the same element set and the same
IERS file (finals2000A.all), and compares:

  TEME position and velocity   SGP4's own output. Skyfield wraps the same Vallado SGP4
                               (python-sgp4), so this checks our inputs to SGP4 and our time
                               since epoch.
  ECEF (ITRF) position         Skyfield's ITRS with UT1 and polar motion from the same file.
                               Checks GMST, UT1 - UTC and polar motion.
  Geodetic lat, lon, height    Skyfield's WGS-84 conversion of its ITRS position.
  J2000 (FK5) position, vel.   Against Skyfield's GCRS. Different models by design (ours is
                               IAU-76/FK5, Skyfield's IAU 2006/2000A plus the frame bias), so
                               the tolerance is metres, not millimetres.
  element_source, element age  The element set the program says it used must be the one the
                               documented rule picks (supplemental where one exists, latest
                               epoch), recomputed here from the JSON files.

Every (satellite, time) of the sample must either have a row or fail in Skyfield's SGP4 too.

Exits 0 if every tolerance holds, 1 otherwise (naming the satellite and time of each
violation), 2 on a usage or input error. Reads only local files; any network access raises.

Usage (inside the CI image, which has the pinned packages):

    python3 python/validate_against_reference.py --propagate build/validate/bin/propagate
"""

from __future__ import annotations

import argparse
import math
import statistics
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import erfa
import numpy as np
from sgp4.api import WGS72, Satrec
from skyfield.api import EarthSatellite, Loader, wgs84
from skyfield.data import iers
from skyfield.framelib import itrs
from skyfield.sgp4lib import TEME

import snapshot_tools as st

# ---------------------------------------------------------------------------------------
# Tolerances. Each is the largest difference accepted for one row, in the unit shown.
# The first three are fixed by the project (docs/phase-1-orbits.md, section 7) and are never
# widened. The others are set a few times above the largest difference measured on arm64 and
# x86-64 (docs/phase-1-results.md), so they hold on both but still catch a real error, which
# shows up as metres to kilometres.
# ---------------------------------------------------------------------------------------
TOLERANCES = {
    # Same SGP4 code and the same inputs on both sides: only rounding differs.
    "teme_position_m": 1e-3,
    # Velocity tolerances are the position tolerance times the orbital angular rate
    # (7.6 km/s / 6928 km = 1.1e-3 per second): an error that rotates or delays the state by
    # the position tolerance moves the velocity by this much. Measured: 2e-6 mm/s.
    "teme_velocity_mm_s": 1.1e-3,
    # The same model (GMST 1982 at UT1, then polar motion) reached by a different route.
    # Measured: under 1 mm (Skyfield also applies the TIO locator s', 0.4 mm at this height,
    # and interpolates the IERS values in TT where we use UTC).
    "ecef_position_m": 1e-2,
    # Geodetic differences converted to metres on the ground: latitude times (6378.137 km +
    # height), longitude times the same times cos(latitude), height as it is. The scale is
    # within 1% of the true radius of curvature, which does not matter for a tolerance.
    # Reference: ERFA's closed-form eraGc2gd (WGS-84) applied to Skyfield's ITRS position, so
    # the values inherit the ECEF difference and share its 1 cm bound. Measured: under 1 mm.
    "latitude_m": 1e-2,
    "longitude_m": 1e-2,
    "height_m": 1e-2,
    # Skyfield's own wgs84.geographic_position_of, also checked. It stops its latitude
    # iteration after three rounds, which leaves up to about 3 mm at Starlink's height
    # (measured: 2.7 mm), so this checks the same thing less tightly. Longitude is the same
    # atan2 in both and is not repeated.
    "skyfield_latitude_m": 1e-2,
    "skyfield_height_m": 1e-2,
    # IAU-76/FK5 J2000 against IAU 2006/2000A GCRS: the frame bias (23 mas, 0.8 m at
    # Starlink's radius) plus model differences. Measured: 1.0 to 1.6 m.
    "j2000_position_m": 5.0,
    # The same rotation applied to the velocity (5 m times 1.1e-3 per second, as above).
    # Measured: under 2 mm/s.
    "j2000_velocity_mm_s": 5.5,
    # Element age (utc - epoch) against the epoch of the element set the rule picks. The
    # program prints the shortest round-trip double; 1 us is far below one SGP4 step's effect
    # (7.6 mm of travel) and far above rounding.
    "element_age_s": 1e-6,
}

UNITS = {
    "teme_position_m": "m",
    "teme_velocity_mm_s": "mm/s",
    "ecef_position_m": "m",
    "latitude_m": "m",
    "longitude_m": "m",
    "height_m": "m",
    "skyfield_latitude_m": "m",
    "skyfield_height_m": "m",
    "j2000_position_m": "m",
    "j2000_velocity_mm_s": "mm/s",
    "element_age_s": "s",
}

# ---------------------------------------------------------------------------------------
# The sample. Deterministic: it depends only on the committed snapshot.
# ---------------------------------------------------------------------------------------
# Times: 40 times every 2 h 0 min 13.75 s from 8 h before the snapshot time (2026-11-18
# 21:15 UTC) to 78 h after the start. The snapshot's supplemental epochs lie between 2026-11-16
# 19:30 and 2026-11-18 19:19, so the first times fall before most satellites' epochs (negative
# element age) and the last ones about three days after it. The odd step puts the times at
# every phase of the day and of the orbit, and on quarter seconds (exact in binary, so both
# sides read the same instant).
SAMPLE_START = "2026-11-18T13:15:00"
SAMPLE_STEP_NS = 7_213_750_000_000  # 7213.75 s
SAMPLE_DURATION_NS = 80 * 3600 * st.NS_PER_S
# Satellites: evenly spread through the catalogue numbers, plus extra six-digit numbers
# (beyond the five digits a TLE can hold), plus the lowest and highest orbits, plus the most
# eccentric ones.
SPREAD_COUNT = 240
SIX_DIGIT_COUNT = 12
LOWEST_COUNT = 15
HIGHEST_COUNT = 5
ECCENTRIC_COUNT = 5

# python-sgp4 refuses catalogue numbers above 339999 (its Alpha-5 limit) and SGP4 never uses
# the number, so every satellite gets this one; the real number stays in this script.
DUMMY_SATNUM = 1
# WGS-72 values SGP4 uses, for the rough perigee height that picks the lowest orbits.
WGS72_MU_KM3_S2 = 398600.8
WGS72_RADIUS_KM = 6378.135
SGP4_EPOCH_JD = 2433281.5  # 1949 December 31 0h, the zero of sgp4init's epoch argument.
EARTH_RADIUS_KM = 6378.137  # WGS-84, for turning angle differences into metres.


def perigee_height_km(record: dict) -> float:
    """Approximate perigee height from the mean elements. Used only to rank orbits."""
    n_rad_s = record["MEAN_MOTION"] * 2.0 * math.pi / 86400.0
    a_km = (WGS72_MU_KM3_S2 / (n_rad_s * n_rad_s)) ** (1.0 / 3.0)
    return a_km * (1.0 - record["ECCENTRICITY"]) - WGS72_RADIUS_KM


def choose_sample(chosen: dict[int, st.ChosenElements]) -> list[int]:
    ordered = sorted(chosen)
    picked = {ordered[(i * len(ordered)) // SPREAD_COUNT] for i in range(SPREAD_COUNT)}
    six_digit = [n for n in ordered if n > 99_999]
    picked |= {
        six_digit[(i * len(six_digit)) // SIX_DIGIT_COUNT]
        for i in range(min(SIX_DIGIT_COUNT, len(six_digit)))
    }
    by_perigee = sorted(ordered, key=lambda n: (perigee_height_km(chosen[n].record), n))
    picked |= set(by_perigee[:LOWEST_COUNT]) | set(by_perigee[-HIGHEST_COUNT:])
    by_eccentricity = sorted(ordered, key=lambda n: (-chosen[n].record["ECCENTRICITY"], n))
    picked |= set(by_eccentricity[:ECCENTRIC_COUNT])
    return sorted(picked)


def sample_times() -> list[st.UtcInstant]:
    start = st.parse_utc(SAMPLE_START)
    count = SAMPLE_DURATION_NS // SAMPLE_STEP_NS + 1
    return [start.plus_ns(i * SAMPLE_STEP_NS) for i in range(count)]


# ---------------------------------------------------------------------------------------
# Skyfield side.
# ---------------------------------------------------------------------------------------
def skyfield_timescale(snapshot: Path):
    """A Skyfield timescale whose UT1 and polar motion come from the snapshot's IERS file.

    Loader(...).timescale(builtin=False) reads finals2000A.all from the snapshot directory;
    it would download the file if it were missing, so its presence is checked first (and
    forbid_network makes a download fail anyway). Polar motion is only applied by Skyfield's
    ITRS frame when ts.polar_motion_table is set, so it is installed from the same file.
    """
    eop_path = snapshot / st.EOP_FILE
    if not eop_path.is_file():
        raise SystemExit(f"error: {eop_path} not found")
    ts = Loader(str(snapshot), verbose=False).timescale(builtin=False)
    with eop_path.open("rb") as f:
        iers.install_polar_motion_table(ts, iers.parse_x_y_dut1_from_finals_all(f))
    return ts


def satrec_for(chosen: st.ChosenElements) -> Satrec:
    """Vallado's SGP4 record for an element set, built from the OMM fields.

    Unit conversions as python-sgp4's own sgp4.omm.initialize. Unlike that function, the
    epoch is not passed through one double count of days (about 0.3 us of rounding, 2 mm
    along track): jdsatepoch and jdsatepochF, from which sgp4() computes the time since
    epoch, are set from the exact integer-nanosecond epoch after sgp4init.
    """
    r = chosen.record
    epoch = chosen.epoch
    deg = math.pi / 180.0
    satrec = Satrec()
    satrec.sgp4init(
        WGS72,
        "i",
        DUMMY_SATNUM,
        (epoch.jd_whole - SGP4_EPOCH_JD) + epoch.jd_fraction,
        r["BSTAR"],
        r.get("MEAN_MOTION_DOT", 0.0) / (1036800.0 / math.pi),
        r.get("MEAN_MOTION_DDOT", 0.0) / (2985984000.0 / 2.0 / math.pi),
        r["ECCENTRICITY"],
        r["ARG_OF_PERICENTER"] * deg,
        r["INCLINATION"] * deg,
        r["MEAN_ANOMALY"] * deg,
        r["MEAN_MOTION"] / 720.0 * math.pi,
        r["RA_OF_ASC_NODE"] * deg,
    )
    satrec.jdsatepoch = epoch.jd_whole
    satrec.jdsatepochF = epoch.jd_fraction
    return satrec


@dataclass
class Reference:
    """Skyfield's values for one satellite at a list of times (arrays, one entry per time)."""

    failed: list[bool]
    teme_km: np.ndarray
    teme_km_s: np.ndarray
    itrs_km: np.ndarray
    latitude_rad: np.ndarray  # ERFA eraGc2gd (WGS-84) of itrs_km.
    longitude_rad: np.ndarray
    height_km: np.ndarray
    skyfield_latitude_rad: np.ndarray  # Skyfield's wgs84.geographic_position_of.
    skyfield_height_km: np.ndarray
    gcrs_km: np.ndarray
    gcrs_km_s: np.ndarray


def skyfield_reference(ts, chosen: st.ChosenElements, times: list[st.UtcInstant]) -> Reference:
    # Calendar fields, never one float Julian date: ts.utc keeps the exact second.
    t = ts.utc(
        [x.year for x in times],
        [x.month for x in times],
        [x.day for x in times],
        [x.hour for x in times],
        [x.minute for x in times],
        [x.second for x in times],
    )
    satellite = EarthSatellite.from_satrec(satrec_for(chosen), ts)
    geocentric = satellite.at(t)
    messages = geocentric.message
    if not isinstance(messages, list):
        messages = [messages] * len(times)
    teme_km, teme_km_s = geocentric.frame_xyz_and_velocity(TEME)
    itrs_km = geocentric.frame_xyz(itrs).km.T
    # ERFA wants metres for its WGS-84 ellipsoid (identifier 1). A failed time is NaN.
    longitude, latitude, height_m = erfa.gc2gd(1, np.nan_to_num(itrs_km) * 1e3)
    geographic = wgs84.geographic_position_of(geocentric)
    return Reference(
        failed=[m is not None for m in messages],
        teme_km=teme_km.km.T,
        teme_km_s=teme_km_s.km_per_s.T,
        itrs_km=itrs_km,
        latitude_rad=latitude,
        longitude_rad=longitude,
        height_km=height_m / 1e3,
        skyfield_latitude_rad=geographic.latitude.radians,
        skyfield_height_km=geographic.elevation.km,
        gcrs_km=geocentric.position.km.T,
        gcrs_km_s=geocentric.velocity.km_per_s.T,
    )


# ---------------------------------------------------------------------------------------
# Comparison.
# ---------------------------------------------------------------------------------------
def vector(row: dict, prefix: str, suffix: str) -> np.ndarray:
    return np.array([row[f"{prefix}_{axis}_{suffix}"] for axis in ("x", "y", "z")])


def velocity(row: dict, prefix: str) -> np.ndarray:
    return np.array([row[f"{prefix}_v{axis}_km_s"] for axis in ("x", "y", "z")])


def wrap_pi(angle: float) -> float:
    return (angle + math.pi) % (2.0 * math.pi) - math.pi


def differences(row: dict, ref: Reference, i: int, chosen: st.ChosenElements) -> dict:
    """Every compared quantity for one row, as a non-negative difference in its unit."""
    km_to_m = 1e3
    km_s_to_mm_s = 1e6
    latitude = math.radians(row["geodetic_latitude_deg"])
    longitude = math.radians(row["geodetic_longitude_deg"])
    radius_m = (EARTH_RADIUS_KM + ref.height_km[i]) * km_to_m
    expected_age_s = st.seconds_between(st.parse_utc(row["utc"]), chosen.epoch)
    return {
        "teme_position_m": km_to_m
        * float(np.linalg.norm(vector(row, "teme", "km") - ref.teme_km[i])),
        "teme_velocity_mm_s": km_s_to_mm_s
        * float(np.linalg.norm(velocity(row, "teme") - ref.teme_km_s[i])),
        "ecef_position_m": km_to_m
        * float(np.linalg.norm(vector(row, "ecef", "km") - ref.itrs_km[i])),
        "latitude_m": abs(latitude - ref.latitude_rad[i]) * radius_m,
        "longitude_m": abs(wrap_pi(longitude - ref.longitude_rad[i]))
        * radius_m
        * math.cos(ref.latitude_rad[i]),
        "height_m": abs(row["geodetic_height_km"] - ref.height_km[i]) * km_to_m,
        "skyfield_latitude_m": abs(latitude - ref.skyfield_latitude_rad[i]) * radius_m,
        "skyfield_height_m": abs(row["geodetic_height_km"] - ref.skyfield_height_km[i]) * km_to_m,
        "j2000_position_m": km_to_m
        * float(np.linalg.norm(vector(row, "j2000", "km") - ref.gcrs_km[i])),
        "j2000_velocity_mm_s": km_s_to_mm_s
        * float(np.linalg.norm(velocity(row, "j2000") - ref.gcrs_km_s[i])),
        "element_age_s": abs(row["element_age_s"] - expected_age_s),
    }


@dataclass
class Outcome:
    """Every difference per quantity, as (value, catalogue number, utc) triples."""

    values: dict[str, list[tuple[float, int, str]]]
    problems: list[str]  # Failures that are not a tolerance: missing or unexpected rows.
    rows: int
    satellites: int
    sgp4_failures: int


def compare(rows: list[dict], snapshot: Path, ids: list[int], times) -> Outcome:
    chosen = st.load_snapshot_elements(snapshot)
    ts = skyfield_timescale(snapshot)
    # Rows keyed by exact time (integer nanoseconds), not by text: the program pads the
    # fraction of a second to a fixed number of digits.
    by_satellite: dict[int, dict[int, dict]] = {}
    problems: list[str] = []
    for row in rows:
        by_satellite.setdefault(row["norad_cat_id"], {})[st.parse_utc(row["utc"]).unix_ns] = row
    unexpected = sorted(set(by_satellite) - set(ids))
    if unexpected:
        problems.append(f"rows for satellites outside the sample: {unexpected[:10]}")

    values: dict[str, list[tuple[float, int, str]]] = {q: [] for q in TOLERANCES}
    sgp4_failures = 0
    for number in ids:
        if number not in chosen:
            problems.append(f"{number}: not in the snapshot")
            continue
        elements = chosen[number]
        ref = skyfield_reference(ts, elements, times)
        satellite_rows = by_satellite.get(number, {})
        extra = sorted(set(satellite_rows) - {x.unix_ns for x in times})
        if extra:
            first = st.utc_from_unix_ns(extra[0]).iso()
            problems.append(f"{number}: rows at times outside the sample, e.g. {first}")
        for i, instant in enumerate(times):
            text = instant.iso()
            row = satellite_rows.get(instant.unix_ns)
            if ref.failed[i]:
                sgp4_failures += 1
                if row is not None:
                    problems.append(f"{number} at {text}: a row, but Skyfield's SGP4 fails")
                continue
            if row is None:
                problems.append(f"{number} at {text}: no row, but Skyfield's SGP4 succeeds")
                continue
            if row["element_source"] != elements.source:
                problems.append(
                    f"{number} at {text}: element_source {row['element_source']}, "
                    f"expected {elements.source}"
                )
            for quantity, value in differences(row, ref, i, elements).items():
                values[quantity].append((value, number, text))
    return Outcome(values, problems, len(rows), len(ids), sgp4_failures)


def report(outcome: Outcome) -> int:
    print(
        f"compared {sum(len(v) for v in outcome.values.values()) // len(TOLERANCES)} rows of "
        f"{outcome.satellites} satellites ({outcome.sgp4_failures} satellite-times fail SGP4 "
        "on both sides)"
    )
    header = f"{'quantity':<22}{'median':>12}{'worst':>12}{'tolerance':>12}  unit  worst at"
    print(header)
    print("-" * len(header))
    violations: list[str] = []
    for quantity, tolerance in TOLERANCES.items():
        entries = outcome.values[quantity]
        if not entries:
            violations.append(f"{quantity}: no values compared")
            continue
        worst = max(entries)
        median = statistics.median(v for v, _, _ in entries)
        status = "" if worst[0] <= tolerance else "  FAIL"
        print(
            f"{quantity:<22}{median:>12.3e}{worst[0]:>12.3e}{tolerance:>12.1e}  "
            f"{UNITS[quantity]:<4}  {worst[1]} {worst[2]}{status}"
        )
        over = sorted((e for e in entries if e[0] > tolerance), reverse=True)
        violations += [
            f"{quantity} = {v:.6g} {UNITS[quantity]} > {tolerance:g} for {n} at {t}"
            for v, n, t in over[:5]
        ]
        if len(over) > 5:
            violations.append(f"{quantity}: ... and {len(over) - 5} more rows over tolerance")
    failures = outcome.problems + violations
    if failures:
        print(f"\nFAIL: {len(failures)} problems", file=sys.stderr)
        for line in failures[:60]:
            print(f"  {line}", file=sys.stderr)
        return 1
    print("\nPASS: every tolerance holds")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run propagate on a fixed sample and compare every row with Skyfield."
    )
    parser.add_argument(
        "--propagate", type=Path, required=True, help="path of the built propagate program"
    )
    parser.add_argument(
        "--snapshot",
        type=Path,
        default=st.DEFAULT_SNAPSHOT,
        help=f"snapshot directory (default: {st.DEFAULT_SNAPSHOT.relative_to(st.REPO_ROOT)})",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    st.forbid_network()
    args = parse_args(argv)
    st.check_snapshot(args.snapshot)
    if not args.propagate.is_file():
        print(f"error: {args.propagate} not found", file=sys.stderr)
        return 2
    chosen = st.load_snapshot_elements(args.snapshot)
    ids = choose_sample(chosen)
    times = sample_times()
    print(
        f"sample: {len(ids)} satellites, {len(times)} times from {times[0].iso()} to "
        f"{times[-1].iso()} UTC"
    )
    with tempfile.TemporaryDirectory(prefix="satsim-validate-") as work:
        csv_path = Path(work) / "sample.csv"
        st.run_propagate(
            args.propagate,
            args.snapshot,
            times[0],
            SAMPLE_DURATION_NS,
            SAMPLE_STEP_NS,
            ids,
            csv_path,
        )
        rows = st.read_propagate_csv(csv_path)
    return report(compare(rows, args.snapshot, ids, times))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

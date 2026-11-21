#!/usr/bin/env python3
"""Write an ideal published shell as a snapshot directory the C++ programs can read.

Two setups (--setup): "hypatia", described first below, and "bariloche", the shell of the
Stability-first paper (arXiv 2408.07460, section IV-B), described after it. Each snapshot
also gets the setup's laser links as a link file (links.csv, the format of
topology/link_file.hpp, docs/phase-3-routing.md 4.9).

The shell is the one of the Hypatia paper (Kassing et al., IMC 2020) and its repository
(github.com/snkas/hypatia): 72 planes of 22 satellites at 53 degrees, written by its generator
satgenpy (satgen/tles/generate_tles_from_scratch.py, called with the constants of
paper/satellite_networks_state/main_starlink_550.py). The element values are theirs:

  - plane o has RAAN o * 360 / 72 degrees;
  - slot s of plane o has mean anomaly s * 360 / 22 degrees, plus half a slot
    (360 / 44 degrees) in every odd plane (their PHASE_DIFF = True);
  - eccentricity 0.0000001, argument of perigee 0, inclination 53 degrees;
  - mean motion 15.19 revolutions per day, no drag term (BSTAR 0);
  - epoch 2000-01-01 00:00:00 UTC (their TLE epoch "00001.00000000"), which is also where
    their simulations start.

Their file says the mean motion means "Altitude ~550 km"; through SGP4 it gives a mean orbit
radius of about 6887 km, so the satellites fly about 510 km above the equator (550 km would
need about 15.05 revolutions per day). Their numbers were computed with these elements, so
they are kept as they are; docs/phase-2-results.md gives the consequences.

The three files the programs read:

  supgp.json       the true element sets of planes 1 to 71, epoch 2000-01-01T00:00:00.
  gp.json          the true element sets of plane 0, which therefore exist only here, and a
                   decoy for every satellite of planes 1 to 71: RAAN 1.25 deg and mean anomaly
                   a quarter slot (360 / 88 deg) further on, epoch one hour LATER than the
                   supplemental set. The documented rule (supplemental set where one exists,
                   whatever the epochs) must pick the true sets for every satellite: a program
                   that preferred the standard file, or the latest epoch across both files,
                   would fly the decoys and fail the cross-check by hundreds of kilometres,
                   and one that ignored the standard file would lose plane 0.
  finals2000A.all  a copy of the committed snapshot's IERS file (it covers 1973 to 2027).

Also written: shells.json, the shell definition for --shells-file (one shell, "hypatia-s1",
53.0 +- 0.3 deg, 495 to 535 km, planes cut at RAAN gaps above 2.5 deg, at least 10 per plane).

Catalogue number of plane o, slot s: 100000 + 22 * o + s (six digits, as many real ones now
are). OBJECT_ID 2025-<o+1>, piece letter A to V by slot: synthetic launches after the last
launch without lasers (2021-044), so every satellite is a laser satellite.

links.csv, Hypatia: satellite s of plane o to satellite s + 1 of plane o and to satellite s of
plane o + 1, planes and slots wrapping (their generate_plus_grid_isls with isl_shift 0):
3,168 links, 4 per satellite.

The "bariloche" setup. The paper and its 2022 companion give: 72 planes of 22, 53 degrees,
550 km, plane o at RAAN o * 5 degrees, satellite i of plane o at o * (360 * 39 / 1584) +
i * (360 / 22) degrees along the orbit (a Walker delta pattern with phase offset F = 39), and
lasers ahead and behind in the plane and to the same index in the next plane. Here:

  - mean motion 86400 / T revolutions per day, with T = 2 pi sqrt(a^3 / mu) for a = 6378.135
    + 550 km and mu = 398600.8 km^3/s^2 (WGS-72, the constants SGP4 uses): T = 5,739 s, the
    paper's "one orbit (5,739 s)". Other elements as for Hypatia (eccentricity 0.0000001,
    argument of perigee 0, BSTAR 0, epoch 2000-01-01 00:00:00 UTC); the mean anomaly is the
    position along the orbit because the orbit is circular and the perigee is at the node;
  - the same catalogue numbers, decoys and file layout as Hypatia; shell "bariloche-s1";
  - links.csv: (o, i) to (o, i + 1), and (o, i) to (o + 1, i) for planes 0 to 70. From plane
    71 the next plane is plane 0 again, one full turn of RAAN later. Continuing the pattern,
    a "plane 72" would put its satellite i at 72 * 360 * 39 / 1584 + i * 360 / 22 = (39 + i)
    * 360 / 22 degrees, which is where satellite (i + 39) mod 22 of plane 0 is. So (71, i)
    links to (0, (i + 39) mod 22) = (0, (i + 17) mod 22): the index grows by 39 at the wrap.
    With that shift every cross-plane link has the same phase step (360 * 39 / 1584 deg),
    which write_links checks. 3,168 links, 4 per satellite.

Deterministic: the same files every run. Standard library only.

Usage:

    python3 python/make_ideal_shell.py --out build/ideal-shell [--setup hypatia|bariloche]
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import string
import sys
from dataclasses import dataclass
from pathlib import Path

import snapshot_tools as st

NUM_ORBS = 72
NUM_SATS_PER_ORB = 22
INCLINATION_DEG = 53.0
ECCENTRICITY = 0.0000001
ARG_OF_PERIGEE_DEG = 0.0
MEAN_MOTION_REV_PER_DAY = 15.19
EPOCH = "2000-01-01T00:00:00.000000"
FIRST_NORAD_CAT_ID = 100_000

# The decoys in gp.json (see the module docstring).
DECOY_EPOCH = "2000-01-01T01:00:00.000000"
DECOY_RAAN_SHIFT_DEG = 1.25
DECOY_MEAN_ANOMALY_SHIFT_DEG = 360.0 / (4 * NUM_SATS_PER_ORB)

SHELL_NAME = "hypatia-s1"
SHELLS_FILE = "shells.json"
LINKS_FILE = "links.csv"
SHELL_DEFINITION = {
    "name": SHELL_NAME,
    "inclination_deg": INCLINATION_DEG,
    "inclination_tolerance_deg": 0.3,
    # Mean height above the WGS-84 ellipsoid over one orbit is about 515 km (measured by the
    # constellation program); the band is wide because nothing else is in this snapshot.
    "min_height_km": 495.0,
    "max_height_km": 535.0,
    # Planes are 5 deg apart in RAAN and a plane's satellites share one RAAN.
    "plane_gap_threshold_deg": 2.5,
    "min_plane_size": 10,
}


# The Bariloche shell (see the module docstring).
WGS72_MU_KM3_S2 = 398600.8
WGS72_RADIUS_KM = 6378.135
BARILOCHE_ALTITUDE_KM = 550.0
BARILOCHE_PHASE_OFFSET = 39
BARILOCHE_PERIOD_S = (
    2.0 * math.pi * math.sqrt((WGS72_RADIUS_KM + BARILOCHE_ALTITUDE_KM) ** 3 / WGS72_MU_KM3_S2)
)


@dataclass(frozen=True)
class Setup:
    """A published shell: its name, mean motion, phasing and laser links."""

    key: str  # The --setup value.
    shell_name: str
    object_prefix: str
    mean_motion_rev_per_day: float
    # Walker phase offset F: plane o is advanced by o * 360 * F / (planes * per plane) deg.
    # None: Hypatia's phasing, half a slot in every odd plane.
    phase_offset: int | None
    min_height_km: float
    max_height_km: float

    def mean_anomaly_deg(self, orbit: int, slot: int) -> float:
        if self.phase_offset is None:
            shift = 360.0 / (NUM_SATS_PER_ORB * 2.0) if orbit % 2 == 1 else 0.0
        else:
            shift = orbit * 360.0 * self.phase_offset / (NUM_ORBS * NUM_SATS_PER_ORB)
        return shift + slot * 360.0 / NUM_SATS_PER_ORB

    def east_slot(self, orbit: int, slot: int) -> tuple[int, int]:
        """The cross-plane partner of (orbit, slot) in the next plane (module docstring)."""
        if orbit + 1 < NUM_ORBS or self.phase_offset is None:
            return (orbit + 1) % NUM_ORBS, slot
        return 0, (slot + self.phase_offset) % NUM_SATS_PER_ORB

    def shell_definition(self) -> dict:
        return {
            **SHELL_DEFINITION,
            "name": self.shell_name,
            "min_height_km": self.min_height_km,
            "max_height_km": self.max_height_km,
        }


HYPATIA = Setup(
    key="hypatia",
    shell_name=SHELL_NAME,
    object_prefix="HYPATIA-S1",
    mean_motion_rev_per_day=MEAN_MOTION_REV_PER_DAY,
    phase_offset=None,
    min_height_km=SHELL_DEFINITION["min_height_km"],
    max_height_km=SHELL_DEFINITION["max_height_km"],
)
BARILOCHE = Setup(
    key="bariloche",
    shell_name="bariloche-s1",
    object_prefix="BARILOCHE-S1",
    mean_motion_rev_per_day=86400.0 / BARILOCHE_PERIOD_S,
    phase_offset=BARILOCHE_PHASE_OFFSET,
    # Mean height above the WGS-84 ellipsoid over one orbit, measured by the constellation
    # program: 556.7 km (mean orbit radius 6928.0 km). The band is wide because nothing else
    # is in this snapshot.
    min_height_km=535.0,
    max_height_km=575.0,
)
SETUPS = {x.key: x for x in (HYPATIA, BARILOCHE)}


@dataclass(frozen=True)
class Slot:
    """One satellite of the shell, by construction."""

    orbit: int  # 0 to 71, ascending RAAN.
    slot: int  # 0 to 21, ascending mean anomaly within the plane.
    setup: Setup = HYPATIA

    @property
    def norad_cat_id(self) -> int:
        return FIRST_NORAD_CAT_ID + NUM_SATS_PER_ORB * self.orbit + self.slot

    @property
    def raan_deg(self) -> float:
        return self.orbit * 360.0 / NUM_ORBS

    @property
    def mean_anomaly_deg(self) -> float:
        return self.setup.mean_anomaly_deg(self.orbit, self.slot)


def all_slots(setup: Setup = HYPATIA) -> list[Slot]:
    return [Slot(o, s, setup) for o in range(NUM_ORBS) for s in range(NUM_SATS_PER_ORB)]


def slot_of(norad_cat_id: int, setup: Setup = HYPATIA) -> Slot:
    index = norad_cat_id - FIRST_NORAD_CAT_ID
    if not 0 <= index < NUM_ORBS * NUM_SATS_PER_ORB:
        raise ValueError(f"{norad_cat_id} is not a satellite of the ideal shell")
    return Slot(index // NUM_SATS_PER_ORB, index % NUM_SATS_PER_ORB, setup)


def links(setup: Setup) -> list[tuple[Slot, Slot]]:
    """The setup's laser links: every in-plane link, then every cross-plane link, once each."""
    n = NUM_SATS_PER_ORB
    in_plane = [
        (Slot(o, s, setup), Slot(o, (s + 1) % n, setup)) for o in range(NUM_ORBS) for s in range(n)
    ]
    cross = [
        (Slot(o, s, setup), Slot(*setup.east_slot(o, s), setup))
        for o in range(NUM_ORBS)
        for s in range(n)
    ]
    return in_plane + cross


def check_links(setup: Setup, pairs: list[tuple[Slot, Slot]]) -> None:
    """Raises SystemExit unless the links are 3,168 distinct pairs, 4 per satellite, and the
    cross-plane links all have the setup's phase step: one value for a Walker pattern (the
    wrap included), +- half a slot for Hypatia."""
    count = NUM_ORBS * NUM_SATS_PER_ORB
    keys = {tuple(sorted((a.norad_cat_id, b.norad_cat_id))) for a, b in pairs}
    degree: dict[int, int] = {}
    for key in keys:
        for x in key:
            degree[x] = degree.get(x, 0) + 1
    if len(keys) != 2 * count or len(degree) != count or set(degree.values()) != {4}:
        raise SystemExit(f"error: {setup.key} links are not {2 * count} pairs, 4 per satellite")
    steps = {
        round((b.mean_anomaly_deg - a.mean_anomaly_deg) % 360.0, 9)
        for a, b in pairs[count:]
        if b.orbit == (a.orbit + 1) % NUM_ORBS
    }
    if len(steps) != (2 if setup.phase_offset is None else 1):
        raise SystemExit(f"error: {setup.key} cross-plane phase steps {sorted(steps)} deg")


def write_links(path: Path, setup: Setup) -> None:
    pairs = links(setup)
    check_links(setup, pairs)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(f"# {setup.shell_name} laser links, written by python/make_ideal_shell.py\n")
        f.write("# every in-plane link, then every cross-plane link (its module docstring)\n")
        f.write("norad_cat_id_a,norad_cat_id_b\n")
        for a, b in pairs:
            f.write(f"{a.norad_cat_id},{b.norad_cat_id}\n")


def omm_record(slot: Slot, epoch: str, raan_deg: float, mean_anomaly_deg: float) -> dict:
    return {
        "OBJECT_NAME": f"{slot.setup.object_prefix}-P{slot.orbit:02d}-S{slot.slot:02d}",
        "OBJECT_ID": f"2025-{slot.orbit + 1:03d}{string.ascii_uppercase[slot.slot]}",
        "EPOCH": epoch,
        "MEAN_MOTION": slot.setup.mean_motion_rev_per_day,
        "ECCENTRICITY": ECCENTRICITY,
        "INCLINATION": INCLINATION_DEG,
        "RA_OF_ASC_NODE": raan_deg % 360.0,
        "ARG_OF_PERICENTER": ARG_OF_PERIGEE_DEG,
        "MEAN_ANOMALY": mean_anomaly_deg % 360.0,
        "EPHEMERIS_TYPE": 0,
        "CLASSIFICATION_TYPE": "U",
        "NORAD_CAT_ID": slot.norad_cat_id,
        "ELEMENT_SET_NO": 999,
        "REV_AT_EPOCH": 0,
        "BSTAR": 0.0,
        "MEAN_MOTION_DOT": 0.0,
        "MEAN_MOTION_DDOT": 0.0,
    }


def true_record(slot: Slot) -> dict:
    return omm_record(slot, EPOCH, slot.raan_deg, slot.mean_anomaly_deg)


def decoy_record(slot: Slot) -> dict:
    return omm_record(
        slot,
        DECOY_EPOCH,
        slot.raan_deg + DECOY_RAAN_SHIFT_DEG,
        slot.mean_anomaly_deg + DECOY_MEAN_ANOMALY_SHIFT_DEG,
    )


def write_json(path: Path, value) -> None:
    with path.open("w", encoding="utf-8") as f:
        json.dump(value, f, indent=1)
        f.write("\n")


def write_ideal_shell(out: Path, eop_source: Path, setup: Setup = HYPATIA) -> None:
    out.mkdir(parents=True, exist_ok=True)
    slots = all_slots(setup)
    supplemental = [true_record(x) for x in slots if x.orbit != 0]
    standard = [true_record(x) for x in slots if x.orbit == 0]
    standard += [decoy_record(x) for x in slots if x.orbit != 0]
    write_json(out / st.SUPPLEMENTAL_FILE, supplemental)
    write_json(out / st.STANDARD_FILE, standard)
    shutil.copyfile(eop_source, out / st.EOP_FILE)
    write_json(out / SHELLS_FILE, {"shells": [setup.shell_definition()]})
    write_links(out / LINKS_FILE, setup)

    # The rule, applied by the Phase 1 helper, must give back exactly the true sets.
    chosen = st.load_snapshot_elements(out)
    if sorted(chosen) != [x.norad_cat_id for x in slots]:
        raise SystemExit("error: the written files do not hold every satellite once")
    for x in slots:
        if chosen[x.norad_cat_id].record != true_record(x):
            raise SystemExit(f"error: the rule does not pick the true set of {x}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Write an ideal published shell snapshot.")
    parser.add_argument("--out", type=Path, required=True, help="directory to write")
    parser.add_argument("--setup", choices=sorted(SETUPS), default=HYPATIA.key)
    parser.add_argument(
        "--eop",
        type=Path,
        default=st.DEFAULT_SNAPSHOT / st.EOP_FILE,
        help="IERS finals2000A.all to copy (default: the committed snapshot's)",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if not args.eop.is_file():
        print(f"error: {args.eop} not found", file=sys.stderr)
        return 2
    setup = SETUPS[args.setup]
    write_ideal_shell(args.out, args.eop, setup)
    print(
        f"wrote {setup.shell_name}: {NUM_ORBS} x {NUM_SATS_PER_ORB} = "
        f"{NUM_ORBS * NUM_SATS_PER_ORB} satellites to {args.out} (supplemental: planes 1-71; "
        f"standard: plane 0 and decoys) and {2 * NUM_ORBS * NUM_SATS_PER_ORB} links to "
        f"{LINKS_FILE}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

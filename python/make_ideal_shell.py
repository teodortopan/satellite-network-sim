#!/usr/bin/env python3
"""Write Hypatia's first Starlink shell as a snapshot directory the C++ programs can read.

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

Deterministic: the same files every run. Standard library only.

Usage:

    python3 python/make_ideal_shell.py --out build/ideal-shell
"""

from __future__ import annotations

import argparse
import json
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


@dataclass(frozen=True)
class Slot:
    """One satellite of the shell, by construction."""

    orbit: int  # 0 to 71, ascending RAAN.
    slot: int  # 0 to 21, ascending mean anomaly within the plane.

    @property
    def norad_cat_id(self) -> int:
        return FIRST_NORAD_CAT_ID + NUM_SATS_PER_ORB * self.orbit + self.slot

    @property
    def raan_deg(self) -> float:
        return self.orbit * 360.0 / NUM_ORBS

    @property
    def mean_anomaly_deg(self) -> float:
        shift = 360.0 / (NUM_SATS_PER_ORB * 2.0) if self.orbit % 2 == 1 else 0.0
        return shift + self.slot * 360.0 / NUM_SATS_PER_ORB


def all_slots() -> list[Slot]:
    return [Slot(o, s) for o in range(NUM_ORBS) for s in range(NUM_SATS_PER_ORB)]


def slot_of(norad_cat_id: int) -> Slot:
    index = norad_cat_id - FIRST_NORAD_CAT_ID
    if not 0 <= index < NUM_ORBS * NUM_SATS_PER_ORB:
        raise ValueError(f"{norad_cat_id} is not a satellite of the ideal shell")
    return Slot(index // NUM_SATS_PER_ORB, index % NUM_SATS_PER_ORB)


def omm_record(slot: Slot, epoch: str, raan_deg: float, mean_anomaly_deg: float) -> dict:
    return {
        "OBJECT_NAME": f"HYPATIA-S1-P{slot.orbit:02d}-S{slot.slot:02d}",
        "OBJECT_ID": f"2025-{slot.orbit + 1:03d}{string.ascii_uppercase[slot.slot]}",
        "EPOCH": epoch,
        "MEAN_MOTION": MEAN_MOTION_REV_PER_DAY,
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


def write_ideal_shell(out: Path, eop_source: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    slots = all_slots()
    supplemental = [true_record(x) for x in slots if x.orbit != 0]
    standard = [true_record(x) for x in slots if x.orbit == 0]
    standard += [decoy_record(x) for x in slots if x.orbit != 0]
    write_json(out / st.SUPPLEMENTAL_FILE, supplemental)
    write_json(out / st.STANDARD_FILE, standard)
    shutil.copyfile(eop_source, out / st.EOP_FILE)
    write_json(out / SHELLS_FILE, {"shells": [SHELL_DEFINITION]})

    # The rule, applied by the Phase 1 helper, must give back exactly the true sets.
    chosen = st.load_snapshot_elements(out)
    if sorted(chosen) != [x.norad_cat_id for x in slots]:
        raise SystemExit("error: the written files do not hold every satellite once")
    for x in slots:
        if chosen[x.norad_cat_id].record != true_record(x):
            raise SystemExit(f"error: the rule does not pick the true set of {x}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Write Hypatia's Starlink shell 1 snapshot.")
    parser.add_argument("--out", type=Path, required=True, help="directory to write")
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
    write_ideal_shell(args.out, args.eop)
    print(
        f"wrote {NUM_ORBS} x {NUM_SATS_PER_ORB} = {NUM_ORBS * NUM_SATS_PER_ORB} satellites to "
        f"{args.out} (supplemental: planes 1-71; standard: plane 0 and decoys)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

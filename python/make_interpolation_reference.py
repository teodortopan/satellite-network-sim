#!/usr/bin/env python3
"""Write reference numbers for the dashboard's interpolation (spec 6.2, last paragraph).

The page interpolates positions between two samples as spec 4.3 says. So that its TypeScript
cannot drift from the Python version the data check uses, this script runs the data check's
own `interpolate` (imported from python/check_dashboard_data.py, not copied) on a few
hand-made sample pairs and shares, and writes the inputs and results to
dashboard/src/globe/interpolation-reference.json. The page's unit test feeds the same inputs
to its own code and compares.

The pairs are made here with plain circle geometry, not with anything from the dashboard:
satellites on circular orbits of a few heights and inclinations, the later sample turned by
the Earth's rotation as an Earth-fixed position would be, plus one pair on the Earth's axis
where both directions are the same (the small-angle branch).

Usage (inside the CI image, which has numpy):

    python3 python/make_interpolation_reference.py
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np

from check_dashboard_data import EARTH_RATE_RAD_S, interpolate, rotate_z

OUT = Path(__file__).resolve().parent.parent / "dashboard/src/globe/interpolation-reference.json"
MU_KM3_S2 = 398600.4418  # Only to give the hand-made orbits a realistic speed.

# (radius km, inclination deg, node deg, phase deg, sample step s).
ORBITS = [
    (6921.0, 53.0, 10.0, 0.0, 60.0),
    (6926.0, 43.0, 200.0, 135.0, 60.0),
    (6931.0, 97.6, 300.0, 80.0, 60.0),
    (6941.0, 70.0, 45.0, 270.0, 1.0),
    (6921.0, 53.0, 120.0, 30.0, 120.0),
]
SHARES = [0.0, 1.0 / 60.0, 0.25, 0.5, 0.9, 1.0]


def ecef_on_circle(radius_km, incl_deg, node_deg, phase_deg, t_s):
    """Earth-fixed position at t_s of a circular orbit whose inertial frame meets the
    Earth-fixed one at t = 0."""
    incl, node = math.radians(incl_deg), math.radians(node_deg)
    u = math.radians(phase_deg) + math.sqrt(MU_KM3_S2 / radius_km**3) * t_s
    cos_u, sin_u = math.cos(u), math.sin(u)
    inertial = radius_km * np.array(
        [
            math.cos(node) * cos_u - math.sin(node) * sin_u * math.cos(incl),
            math.sin(node) * cos_u + math.cos(node) * sin_u * math.cos(incl),
            sin_u * math.sin(incl),
        ]
    )
    return rotate_z(inertial[None, :], -EARTH_RATE_RAD_S * t_s)[0]


def main() -> None:
    pairs = []
    for radius, incl, node, phase, step in ORBITS:
        p0 = ecef_on_circle(radius, incl, node, phase, 0.0)
        p1 = ecef_on_circle(radius, incl, node, phase, step)
        pairs.append((p0, p1, step))
    pole = np.array([0.0, 0.0, 6921.0])
    pairs.append((pole, pole.copy(), 60.0))

    cases = []
    for p0, p1, step in pairs:
        for share in SHARES:
            out = interpolate(p0[None, :], p1[None, :], step, share)[0]
            cases.append(
                {
                    "p0_km": [float(v) for v in p0],
                    "p1_km": [float(v) for v in p1],
                    "dt_s": step,
                    "share": share,
                    "expected_km": [float(v) for v in out],
                }
            )
    document = {
        "source": "python/make_interpolation_reference.py, using interpolate() of "
        "python/check_dashboard_data.py",
        "earth_rate_rad_s": EARTH_RATE_RAD_S,
        "cases": cases,
    }
    OUT.write_text(json.dumps(document, indent=1) + "\n")
    print(f"wrote {len(cases)} cases to {OUT}")


if __name__ == "__main__":
    main()

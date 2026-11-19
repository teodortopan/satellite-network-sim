#!/usr/bin/env python3
"""Write assumed_gateways.json: a gateway at every PoP that has no real one nearby.

The community gateway list (gateways.json) is built from FCC filings and holds sites in the
United States and its territories only. SpaceX has gateways on every continent it serves,
and builds them near its points of presence (PoPs). Without them, traffic of a user outside
the United States could only come down in the United States.

This script reads one ground-data directory and writes assumed_gateways.json into it, in
the format of gateways.json: one gateway at the coordinates of each PoP in pop_codes.json
that has no operational gateway of gateways.json within --radius-km (great circle on the
mean sphere). These gateways are an assumption, not data: every row says so in its name,
and the simulator reports them separately from the real ones.

Standard library only. Reads and writes local files; no network access.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

# Mean radius of the WGS-84 ellipsoid, (2a + b) / 3, as in satsim::ground.
EARTH_MEAN_RADIUS_KM = 6371.0087714
DEFAULT_RADIUS_KM = 500.0


def great_circle_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Great-circle distance between two points given in degrees."""
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    delta_lon = math.radians(lon2 - lon1)
    y = math.hypot(
        math.cos(phi2) * math.sin(delta_lon),
        math.cos(phi1) * math.sin(phi2) - math.sin(phi1) * math.cos(phi2) * math.cos(delta_lon),
    )
    x = math.sin(phi1) * math.sin(phi2) + math.cos(phi1) * math.cos(phi2) * math.cos(delta_lon)
    return EARTH_MEAN_RADIUS_KM * math.atan2(y, x)


def assumed_gateways(pops: list[dict], gateways: list[dict], radius_km: float) -> list[dict]:
    """One assumed gateway per distinct PoP site with no operational gateway within radius_km."""
    operational = [g for g in gateways if g["status"] == "operational"]
    result: list[dict] = []
    seen: set[tuple[float, float]] = set()
    for pop in sorted(pops, key=lambda p: p["code"]):
        site = (pop["lat"], pop["lon"])
        if site in seen:  # Two PoP codes at one site (atl and atl2) need one gateway.
            continue
        seen.add(site)
        nearest = min(great_circle_km(*site, g["lat"], g["lon"]) for g in operational)
        if nearest > radius_km:
            place = f"{pop['city']}, {pop['country']}"
            result.append(
                {
                    "name": f"Assumed gateway at PoP {pop['code']} ({place})",
                    "lat": pop["lat"],
                    "lon": pop["lon"],
                    "status": "operational",
                }
            )
    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ground_dir", type=Path, help="a dated directory under data/ground")
    parser.add_argument(
        "--radius-km",
        type=float,
        default=DEFAULT_RADIUS_KM,
        help="a PoP with an operational gateway within this distance gets no assumed one "
        f"(default: {DEFAULT_RADIUS_KM:g})",
    )
    args = parser.parse_args(argv)
    if not args.radius_km > 0.0:
        parser.error("--radius-km must be positive")

    pops = json.loads((args.ground_dir / "pop_codes.json").read_text(encoding="utf-8"))
    gateways = json.loads((args.ground_dir / "gateways.json").read_text(encoding="utf-8"))
    result = assumed_gateways(pops, gateways, args.radius_km)
    out = args.ground_dir / "assumed_gateways.json"
    out.write_text(json.dumps(result, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{out}: {len(result)} assumed gateways for {len(pops)} PoP codes")
    for gateway in result:
        print(f"  {gateway['name']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

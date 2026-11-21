#!/usr/bin/env python3
"""Convert Hypatia's 100 cities into this project's cities.json format.

Input: Hypatia's ground_stations_cities_sorted_by_estimated_2025_pop_top_100.basic.txt (from
github.com/snkas/hypatia, paper/satellite_networks_state/input_data/, MIT licence), lines
"id,name,latitude,longitude,elevation_m" with ids 0 to 99. Output (data/ground/hypatia/
cities.json): the 100 cities in id order, with Hypatia's name, latitude and longitude as
given, and the 4,950 pairs (i, j), i < j, in Hypatia's order (i, then j), from i to j, role
"validation". Hypatia counts each pair once, from the lower id (analyze_path.py), which
`routing --directions forward` reproduces.

The loader needs country, region and geofeed_city for the home-PoP lookup of the realistic
model; Hypatia's setup is best case only, so they are "ZZ" (the ISO 3166 code for an unknown
country), "ZZ" and the name. Every elevation in the file is 0, which is also the height this
project gives every city.

Usage:

    python3 python/make_hypatia_cities.py --basic <file> --out data/ground/hypatia/cities.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def convert(lines: list[str]) -> dict:
    cities = []
    for number, line in enumerate(lines):
        fields = line.rstrip("\n").split(",")
        if len(fields) != 5 or int(fields[0]) != number or float(fields[4]) != 0.0:
            raise SystemExit(f"error: line {number + 1} is not 'id,name,lat,lon,0': {line!r}")
        name, lat, lon = fields[1], float(fields[2]), float(fields[3])
        cities.append(
            {"name": name, "country": "ZZ", "region": "ZZ", "geofeed_city": name, "lat": lat,
             "lon": lon}
        )  # fmt: skip
    if len({c["name"] for c in cities}) != len(cities):
        raise SystemExit("error: duplicate city names")
    pairs = [
        {"from": a["name"], "to": b["name"], "role": "validation", "why": f"Hypatia {i}-{j}"}
        for i, a in enumerate(cities)
        for j, b in enumerate(cities)
        if i < j
    ]
    return {
        "note": (
            "Hypatia's 100 cities (github.com/snkas/hypatia, MIT licence), converted by "
            "python/make_hypatia_cities.py; see README.md here."
        ),
        "cities": cities,
        "pairs": pairs,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--basic", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    with args.basic.open(encoding="utf-8") as f:
        data = convert([line for line in f if line.strip()])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"wrote {len(data['cities'])} cities and {len(data['pairs'])} pairs to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

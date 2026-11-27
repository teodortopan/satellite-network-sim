#!/usr/bin/env python3
"""Download the list of places a visitor of the dashboard can search by name.

One run saves, under <out>/ (default data/ground/places/):

  places.json     One row per place: name, region, country, latitude, longitude, population.
                  Largest population first; 0 where Natural Earth has none.
  metadata.json   source URL, UTC fetch time, sha256 and size of the response, the licence
                  and the number of rows.

The source is Natural Earth's "populated places" (1:10m, simple attributes), public domain,
read as GeoJSON from the natural-earth-vector repository at a fixed release tag. It is the
same project the dashboard's land outlines come from. It is a map maker's selection of about
7,300 cities and towns, not a full gazetteer: a place that is missing can still be chosen on
the dashboard by clicking the globe.

Only the fields the dashboard shows are kept. Latitude and longitude are WGS-84 degrees,
taken from each feature's point and rounded to four decimals (about 11 m).

Download rules, the same as fetch_elements.py: the URL is requested exactly once, nothing is
retried, and the first error stops the run with nothing written.

Run by hand, never by CI or tests. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from fetch_elements import FetchError, fetch, file_metadata

RELEASE = "v5.1.2"
SOURCE_URL = (
    "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    f"{RELEASE}/geojson/ne_10m_populated_places_simple.geojson"
)
LICENCE = "public domain (Natural Earth terms of use)"
FIELDS = ("name", "region", "country", "lat_deg", "lon_deg", "population")
MIN_PLACES = 5000  # Far below the real count; guards against a cut-off response.

DEFAULT_OUT = Path(__file__).resolve().parent.parent / "data" / "ground" / "places"


def text(properties: dict, key: str) -> str:
    value = properties.get(key)
    return value.strip() if isinstance(value, str) else ""


def place_row(url: str, feature: object) -> list:
    """One feature as a row in the order of FIELDS. Raises FetchError if it is not a place."""
    if not isinstance(feature, dict):
        raise FetchError(f"{url}: a feature is not an object: {feature!r}")
    properties, geometry = feature.get("properties"), feature.get("geometry")
    if not isinstance(properties, dict) or not isinstance(geometry, dict):
        raise FetchError(f"{url}: a feature lacks properties or geometry: {feature!r}")
    coordinates = geometry.get("coordinates")
    if (
        geometry.get("type") != "Point"
        or not isinstance(coordinates, list)
        or len(coordinates) != 2
        or not all(isinstance(c, (int, float)) for c in coordinates)
    ):
        raise FetchError(f"{url}: a feature is not a point: {geometry!r}")
    lon, lat = coordinates
    if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
        raise FetchError(f"{url}: coordinates out of range: {geometry!r}")
    name = text(properties, "name")
    country = text(properties, "adm0name")
    if not name or not country:
        raise FetchError(f"{url}: a place has no name or no country: {properties!r}")
    population = properties.get("pop_max")
    if not isinstance(population, (int, float)):
        raise FetchError(f"{url}: a place has no population field: {properties!r}")
    # Natural Earth writes an unknown population as -99; it is kept as 0 and sorts last.
    known = max(int(population), 0)
    return [name, text(properties, "adm1name"), country, round(lat, 4), round(lon, 4), known]


def places_from(url: str, body: bytes) -> list[list]:
    try:
        parsed = json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        preview = body[:200].decode("utf-8", errors="replace")
        raise FetchError(f"{url}: body is not JSON ({e}); starts with: {preview!r}") from e
    features = parsed.get("features") if isinstance(parsed, dict) else None
    if not isinstance(features, list):
        raise FetchError(f"{url}: no list of features")
    rows = [place_row(url, feature) for feature in features]
    if len(rows) < MIN_PLACES:
        raise FetchError(f"{url}: {len(rows)} places, fewer than the {MIN_PLACES} expected")
    # Largest first; name, country and coordinates break ties so the order never depends on
    # the order of the source file.
    rows.sort(key=lambda r: (-r[5], r[0], r[2], r[3], r[4]))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()

    try:
        body, fetched_at = fetch(SOURCE_URL)
        rows = places_from(SOURCE_URL, body)
    except FetchError as e:
        print(f"fetch_places: {e}", file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    places = {"fields": list(FIELDS), "rows": rows}
    (args.out / "places.json").write_text(
        json.dumps(places, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8"
    )
    metadata = {
        "places.json": file_metadata(SOURCE_URL, body, fetched_at)
        | {"dataset": "Natural Earth populated places, 1:10m, simple", "release": RELEASE}
        | {"licence": LICENCE, "rows": len(rows)}
    }
    (args.out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"places.json: {len(rows)} places from {len(body)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())

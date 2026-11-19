#!/usr/bin/env python3
"""Download one dated set of Starlink ground data: gateways and points of presence.

One run saves, under <out>/<YYYY-MM-DD>/ (UTC date):

  starlink_pops.csv       SpaceX's own file: address block, router name, PoP code. As received.
  starlink_geofeed.csv    SpaceX's own location feed: address block, country, region, city.
                          As received. With starlink_pops.csv it gives each city's home PoP.
  gateways.json           Gateway ground stations (name, latitude, longitude, status).
  pop_locations.json      PoP locations (code, city, country, latitude, longitude).
  metadata.json           source URL, UTC fetch time, sha256 and size of every response,
                          and the licence and revision of the community dataset

SpaceX publishes no list of gateway sites. gateways.json and pop_locations.json come from a
community dataset (juliensimon/starlink-ground-stations on Hugging Face, CC-BY-4.0), built
from FCC earth-station filings, Starlink Insider and PeeringDB. It is unofficial: sites may
be missing, planned or retired. The two JSON files are that dataset's rows, unchanged, read
through the Hugging Face rows interface (100 rows per request).

Download rules, the same as fetch_elements.py: each URL is requested exactly once, nothing
is retried, and the first error stops the run with nothing written.

Run by hand, never by CI or tests. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import urllib.parse
from pathlib import Path

from fetch_elements import FetchError, fetch, file_metadata, utc_now

SPACEX_SOURCES = (
    ("starlink_pops.csv", "https://geoip.starlinkisp.net/pops.csv"),
    ("starlink_geofeed.csv", "https://geoip.starlinkisp.net/feed.csv"),
)

DATASET = "juliensimon/starlink-ground-stations"
DATASET_LICENCE = "CC-BY-4.0"
DATASET_INFO_URL = f"https://huggingface.co/api/datasets/{DATASET}"
DATASET_ROWS_URL = "https://datasets-server.huggingface.co/rows"
ROWS_PER_REQUEST = 100  # The interface's maximum.
MAX_ROWS = 5000  # Far above the real size; guards against a runaway loop.

# Output file, dataset subset, fields every row must have.
DATASET_SUBSETS = (
    ("gateways.json", "gateways", ("name", "lat", "lon", "status")),
    ("pop_locations.json", "pops", ("code", "city", "country", "lat", "lon")),
)

DEFAULT_OUT = Path(__file__).resolve().parent.parent / "data" / "ground"


def parse_json(url: str, body: bytes) -> dict:
    try:
        parsed = json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        preview = body[:200].decode("utf-8", errors="replace")
        raise FetchError(f"{url}: body is not JSON ({e}); starts with: {preview!r}") from e
    if not isinstance(parsed, dict):
        raise FetchError(f"{url}: top-level JSON value is {type(parsed).__name__}, not an object")
    return parsed


def check_csv(url: str, body: bytes, min_fields: int) -> int:
    """Checks that body is text with at least min_fields comma-separated fields per line."""
    try:
        lines = [line for line in body.decode("utf-8").splitlines() if line.strip()]
    except UnicodeDecodeError as e:
        raise FetchError(f"{url}: body is not UTF-8 text ({e})") from e
    if not lines:
        raise FetchError(f"{url}: file is empty")
    for number, line in enumerate(lines, start=1):
        if len(line.split(",")) < min_fields or "/" not in line.split(",")[0]:
            raise FetchError(f"{url}: line {number} is not an address-block row: {line[:80]!r}")
    return len(lines)


def check_coordinates(url: str, row: dict) -> None:
    lat, lon = row["lat"], row["lon"]
    numbers = isinstance(lat, (int, float)) and isinstance(lon, (int, float))
    if not numbers or not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
        raise FetchError(f"{url}: row has no valid latitude and longitude: {row!r}")


def fetch_subset(subset: str, fields: tuple[str, ...]) -> tuple[list[dict], list[dict]]:
    """Reads every row of one dataset subset. Returns (rows, metadata of each response)."""
    rows: list[dict] = []
    responses: list[dict] = []
    total = None
    while total is None or len(rows) < total:
        query = urllib.parse.urlencode(
            {
                "dataset": DATASET,
                "config": subset,
                "split": "train",
                "offset": len(rows),
                "length": ROWS_PER_REQUEST,
            }
        )
        url = f"{DATASET_ROWS_URL}?{query}"
        body, fetched_at = fetch(url)
        page = parse_json(url, body)
        total = page.get("num_rows_total")
        page_rows = page.get("rows")
        if not isinstance(total, int) or not isinstance(page_rows, list) or not page_rows:
            raise FetchError(f"{url}: unexpected response layout")
        if total > MAX_ROWS:
            raise FetchError(f"{url}: {total} rows, more than the {MAX_ROWS} allowed")
        for entry in page_rows:
            row = entry.get("row") if isinstance(entry, dict) else None
            if not isinstance(row, dict) or any(field not in row for field in fields):
                raise FetchError(f"{url}: a row lacks one of {fields}: {entry!r}")
            check_coordinates(url, row)
            rows.append(row)
        responses.append(file_metadata(url, body, fetched_at))
    if len(rows) != total:
        raise FetchError(f"{DATASET} {subset}: read {len(rows)} rows, expected {total}")
    return rows, responses


def download_ground_data(work_dir: Path) -> dict:
    """Downloads everything into work_dir and returns the metadata. Raises FetchError."""
    metadata: dict = {}

    for name, url in SPACEX_SOURCES:
        body, fetched_at = fetch(url)
        rows = check_csv(url, body, min_fields=3)
        (work_dir / name).write_bytes(body)
        metadata[name] = file_metadata(url, body, fetched_at) | {"rows": rows}
        print(f"{name}: {len(body)} bytes, {rows} rows")

    body, fetched_at = fetch(DATASET_INFO_URL)
    info = parse_json(DATASET_INFO_URL, body)
    revision, last_modified = info.get("sha"), info.get("lastModified")
    if not isinstance(revision, str) or not isinstance(last_modified, str):
        raise FetchError(f"{DATASET_INFO_URL}: no revision or modification time")
    dataset = {
        "dataset": DATASET,
        "licence": DATASET_LICENCE,
        "attribution": "Julien Simon, Starlink Ground Stations & Points of Presence",
        "revision": revision,
        "last_modified": last_modified,
        "official": False,
    }

    for name, subset, fields in DATASET_SUBSETS:
        rows, responses = fetch_subset(subset, fields)
        text = json.dumps(rows, indent=1, ensure_ascii=False) + "\n"
        (work_dir / name).write_text(text, encoding="utf-8")
        metadata[name] = dataset | {"subset": subset, "rows": len(rows), "responses": responses}
        print(f"{name}: {len(rows)} rows")

    return metadata


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Download Starlink ground data: SpaceX's PoP and location files, and an "
            "unofficial community list of gateways and PoP locations. Each URL is requested "
            "once and nothing is retried."
        )
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help="directory that receives the <YYYY-MM-DD> directory "
        "(default: data/ground in the repository)",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    today = utc_now().date()
    final_dir: Path = args.out / today.isoformat()
    # Downloaded into a working directory that is renamed only on success, so a failure
    # never leaves a partial set behind.
    work_dir: Path = args.out / f".{today.isoformat()}.partial"
    for path in (final_dir, work_dir):
        if path.exists():
            print(f"error: {path} already exists; refusing to overwrite", file=sys.stderr)
            return 1

    work_dir.mkdir(parents=True)
    try:
        metadata = download_ground_data(work_dir)
    except FetchError as e:
        shutil.rmtree(work_dir)
        print(f"error: {e}", file=sys.stderr)
        print("error: nothing was kept", file=sys.stderr)
        return 1
    except BaseException:
        shutil.rmtree(work_dir, ignore_errors=True)
        raise

    (work_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    work_dir.rename(final_dir)
    print(f"wrote {final_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

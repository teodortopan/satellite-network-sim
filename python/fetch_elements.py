#!/usr/bin/env python3
"""Download one snapshot of Starlink GP data (OMM JSON) from CelesTrak.

Writes <out>/<YYYY-MM-DD>/gp.json and supgp.json exactly as received, plus metadata.json
(source URL, UTC fetch time, sha256, byte size and record count of each file).

CelesTrak allows one download per data update (about every two hours) per group; a second
request gets HTTP 403, and clients that keep making failing requests get their IP blocked.
So this script requests each URL exactly once, never retries, and stops at the first
error. It is run by hand, never by CI or tests. Standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

SOURCES = (
    ("gp.json", "https://celestrak.org/NORAD/elements/gp.php?GROUP=starlink&FORMAT=json"),
    (
        "supgp.json",
        "https://celestrak.org/NORAD/elements/supplemental/sup-gp.php?FILE=starlink&FORMAT=json",
    ),
)

# Generic on purpose: nothing that identifies the user or the project.
USER_AGENT = "Python-urllib fetch_elements"
TIMEOUT_S = 60.0
DEFAULT_OUT = Path(__file__).resolve().parent.parent / "data" / "starlink"


class FetchError(Exception):
    """A download or validation failure; the message is shown to the user."""


def fetch(url: str) -> tuple[bytes, str]:
    """Requests url once. Returns the body and the UTC time of the request."""
    fetched_at = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_S) as response:
            if response.status != 200:
                raise FetchError(f"{url}: HTTP {response.status} {response.reason}")
            body = response.read()
    except urllib.error.HTTPError as e:
        raise FetchError(f"{url}: HTTP {e.code} {e.reason}") from e
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise FetchError(f"{url}: request failed: {e}") from e
    return body, fetched_at


def count_records(url: str, body: bytes) -> int:
    """Checks that body is a non-empty JSON array and returns its length."""
    try:
        parsed = json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        preview = body[:200].decode("utf-8", errors="replace")
        raise FetchError(f"{url}: body is not JSON ({e}); starts with: {preview!r}") from e
    if not isinstance(parsed, list):
        raise FetchError(f"{url}: top-level JSON value is {type(parsed).__name__}, not an array")
    if not parsed:
        raise FetchError(f"{url}: JSON array is empty")
    return len(parsed)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Download the CelesTrak Starlink standard and supplemental GP data (OMM JSON) "
            "once each and save them as a dated snapshot. Never retries: CelesTrak blocks "
            "clients that repeat requests. Run at most once per data update (about 2 hours)."
        )
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help="directory that receives the <YYYY-MM-DD> snapshot directory "
        "(default: data/starlink in the repository)",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    today = datetime.datetime.now(datetime.UTC).date().isoformat()
    snapshot_dir: Path = args.out / today
    if snapshot_dir.exists():
        print(f"error: {snapshot_dir} already exists; refusing to overwrite", file=sys.stderr)
        return 1

    # Everything is downloaded and checked before anything is written, so a failure never
    # leaves a partial snapshot behind.
    downloads = []
    try:
        for name, url in SOURCES:
            body, fetched_at = fetch(url)
            records = count_records(url, body)
            downloads.append((name, url, body, fetched_at, records))
            print(f"{name}: {len(body)} bytes, {records} records")
    except FetchError as e:
        print(f"error: {e}", file=sys.stderr)
        print("error: nothing was written; do not retry before the next update", file=sys.stderr)
        return 1

    snapshot_dir.mkdir(parents=True)
    metadata = {}
    for name, url, body, fetched_at, records in downloads:
        (snapshot_dir / name).write_bytes(body)
        metadata[name] = {
            "source_url": url,
            "fetched_at_utc": fetched_at,
            "sha256": hashlib.sha256(body).hexdigest(),
            "bytes": len(body),
            "records": records,
        }
    (snapshot_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"wrote {snapshot_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

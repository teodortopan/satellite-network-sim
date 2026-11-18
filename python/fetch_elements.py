#!/usr/bin/env python3
"""Download one matched snapshot of Starlink orbit data.

One run saves, under <out>/<YYYY-MM-DD>/ (UTC date):

  gp.json            standard GP element sets from CelesTrak (OMM JSON), as received
  supgp.json         supplemental GP element sets from CelesTrak (OMM JSON), as received
  finals2000A.all    IERS Earth-orientation data (UT1-UTC and polar motion), as received
  ephemerides/       SpaceX's own predicted trajectories for a fixed sample of satellites
  metadata.json      source URL, UTC fetch time, sha256 and size of every file above

Everything is taken in one run because the trajectory files only cover the next two days
and are replaced several times a day, with no archive: a comparison between element sets and
trajectories only means something if both were current at the same time.

The ephemerides directory is ignored by git (large, and the site states no redistribution
terms); metadata.json records the checksum of each file in it.

Download rules, the same for every host: each URL is requested exactly once, nothing is
retried, and the first unexpected error stops the run with nothing written. CelesTrak
allows one download per data update (about every two hours) per group, answers a second
request with HTTP 403, and blocks clients that keep making failing requests. It is
therefore contacted last, after everything else has succeeded. The one tolerated failure is
a trajectory file that has been replaced between reading the manifest and requesting it:
that satellite is skipped and recorded.

Run by hand, never by CI or tests. Standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

CELESTRAK_SOURCES = (
    ("gp.json", "https://celestrak.org/NORAD/elements/gp.php?GROUP=starlink&FORMAT=json"),
    (
        "supgp.json",
        "https://celestrak.org/NORAD/elements/supplemental/sup-gp.php?FILE=starlink&FORMAT=json",
    ),
)
EOP_NAME = "finals2000A.all"
EOP_URL = "https://datacenter.iers.org/data/9/finals2000A.all"
EPHEMERIS_BASE_URL = "https://api.starlink.com/public-files/ephemerides/"
EPHEMERIS_MANIFEST_URL = EPHEMERIS_BASE_URL + "MANIFEST.txt"
EPHEMERIS_DIR = "ephemerides"

# MEME_<catalog number>_<name>_<start DDDHHMM>_Operational_<GPS seconds>_UNCLASSIFIED.txt
# MEME is the frame: mean equator and mean equinox of J2000.
EPHEMERIS_NAME = re.compile(r"^MEME_(\d+)_([A-Za-z0-9-]+)_\d+_Operational_\d+_UNCLASSIFIED\.txt$")
# First line of a trajectory file, e.g. "created:2026-11-18 13:17:31 UTC".
EPHEMERIS_FIRST_LINE = re.compile(rb"^created:\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} UTC\s*$")

# Generic on purpose: nothing that identifies the user or the project.
USER_AGENT = "Python-urllib fetch_elements"
TIMEOUT_S = 120.0
# Pause between trajectory downloads, to stay a polite client.
EPHEMERIS_PAUSE_S = 0.2
# More skipped trajectory files than this fraction means something is wrong, not a race.
MAX_SKIPPED_FRACTION = 0.2
DEFAULT_EPHEMERIS_COUNT = 150
DEFAULT_OUT = Path(__file__).resolve().parent.parent / "data" / "starlink"


class FetchError(Exception):
    """A download or validation failure; the message is shown to the user."""


class NotFoundError(FetchError):
    """HTTP 404: the one failure that is tolerated, and only for trajectory files."""


def utc_now() -> datetime.datetime:
    return datetime.datetime.now(datetime.UTC)


def fetch(url: str) -> tuple[bytes, str]:
    """Requests url once. Returns the body and the UTC time of the request."""
    fetched_at = utc_now().strftime("%Y-%m-%dT%H:%M:%SZ")
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_S) as response:
            if response.status != 200:
                raise FetchError(f"{url}: HTTP {response.status} {response.reason}")
            body = response.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise NotFoundError(f"{url}: HTTP 404 {e.reason}") from e
        raise FetchError(f"{url}: HTTP {e.code} {e.reason}") from e
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise FetchError(f"{url}: request failed: {e}") from e
    return body, fetched_at


def file_metadata(url: str, body: bytes, fetched_at: str) -> dict:
    return {
        "source_url": url,
        "fetched_at_utc": fetched_at,
        "sha256": hashlib.sha256(body).hexdigest(),
        "bytes": len(body),
    }


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


def modified_julian_date(date: datetime.date) -> int:
    return date.toordinal() - datetime.date(1858, 11, 17).toordinal()


def check_eop_covers(url: str, body: bytes, date: datetime.date) -> None:
    """Checks that the IERS file has UT1-UTC and polar motion for date and four days after.

    Row layout (fixed width): MJD in columns 8-15, polar motion x and y in arcseconds in
    columns 19-27 and 38-46, UT1-UTC in seconds in columns 59-68.
    """
    wanted = {modified_julian_date(date) + offset for offset in range(5)}
    for line in body.decode("ascii", errors="replace").splitlines():
        try:
            mjd = float(line[7:15])
            float(line[18:27]), float(line[37:46]), float(line[58:68])
        except ValueError:
            continue
        wanted.discard(round(mjd))
    if wanted:
        raise FetchError(f"{url}: no Earth-orientation values for MJD {sorted(wanted)}")


def choose_ephemerides(manifest: bytes, count: int) -> tuple[list[tuple[int, str]], int]:
    """Picks count trajectory files, evenly spaced through the manifest by catalog number.

    Returns the chosen (catalog number, file name) pairs and the number of usable entries.
    The choice depends only on the manifest, so it can be reproduced from its checksum.
    """
    entries: dict[int, str] = {}
    for raw in manifest.decode("ascii", errors="replace").split():
        match = EPHEMERIS_NAME.match(raw)
        if match:
            entries[int(match.group(1))] = raw
    if len(entries) < count:
        raise FetchError(
            f"{EPHEMERIS_MANIFEST_URL}: {len(entries)} usable entries, fewer than the "
            f"{count} requested"
        )
    ordered = sorted(entries.items())
    chosen = [ordered[(i * len(ordered)) // count] for i in range(count)]
    return chosen, len(ordered)


def ephemeris_header(body: bytes) -> dict | None:
    """Returns the header fields of a trajectory file, or None if it is not one."""
    lines = body[:400].split(b"\n")
    if len(lines) < 3 or not EPHEMERIS_FIRST_LINE.match(lines[0]):
        return None
    header = {"created": lines[0].decode("ascii").strip().removeprefix("created:")}
    match = re.match(
        rb"^ephemeris_start:(.+ UTC) ephemeris_stop:(.+ UTC) step_size:(\d+)\s*$", lines[1]
    )
    if not match:
        return None
    header["ephemeris_start"] = match.group(1).decode("ascii")
    header["ephemeris_stop"] = match.group(2).decode("ascii")
    header["step_size_s"] = int(match.group(3))
    return header


def fetch_ephemerides(chosen: list[tuple[int, str]], directory: Path) -> tuple[list, list]:
    """Downloads the chosen trajectory files into directory. Returns (saved, skipped)."""
    directory.mkdir()
    saved, skipped = [], []
    for index, (norad_cat_id, name) in enumerate(chosen, start=1):
        if index > 1:
            time.sleep(EPHEMERIS_PAUSE_S)
        url = EPHEMERIS_BASE_URL + name
        try:
            body, fetched_at = fetch(url)
        except NotFoundError:
            skipped.append({"norad_cat_id": norad_cat_id, "file": name, "reason": "HTTP 404"})
            continue
        header = ephemeris_header(body)
        if header is None:
            skipped.append(
                {"norad_cat_id": norad_cat_id, "file": name, "reason": "not a trajectory file"}
            )
            continue
        (directory / name).write_bytes(body)
        saved.append(
            {"norad_cat_id": norad_cat_id, "file": name}
            | file_metadata(url, body, fetched_at)
            | header
        )
        if index % 25 == 0 or index == len(chosen):
            print(f"ephemerides: {index}/{len(chosen)} requested, {len(saved)} saved")
    if len(skipped) > MAX_SKIPPED_FRACTION * len(chosen):
        raise FetchError(f"{len(skipped)} of {len(chosen)} trajectory files could not be saved")
    return saved, skipped


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Download a matched Starlink snapshot: CelesTrak standard and supplemental GP "
            "data (OMM JSON), the IERS Earth-orientation file, and SpaceX trajectory files "
            "for a sample of satellites. Each URL is requested once and nothing is retried: "
            "CelesTrak blocks clients that repeat requests. Run at most once per CelesTrak "
            "data update (about 2 hours)."
        )
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help="directory that receives the <YYYY-MM-DD> snapshot directory "
        "(default: data/starlink in the repository)",
    )
    parser.add_argument(
        "--ephemeris-count",
        type=int,
        default=DEFAULT_EPHEMERIS_COUNT,
        help=f"number of trajectory files to download (default: {DEFAULT_EPHEMERIS_COUNT}; "
        "each is about 1.5 MB)",
    )
    args = parser.parse_args(argv)
    if args.ephemeris_count < 1:
        parser.error("--ephemeris-count must be at least 1")
    return args


def download_snapshot(work_dir: Path, today: datetime.date, ephemeris_count: int) -> dict:
    """Downloads everything into work_dir and returns the metadata. Raises FetchError."""
    metadata: dict = {}

    body, fetched_at = fetch(EOP_URL)
    check_eop_covers(EOP_URL, body, today)
    (work_dir / EOP_NAME).write_bytes(body)
    metadata[EOP_NAME] = file_metadata(EOP_URL, body, fetched_at)
    print(f"{EOP_NAME}: {len(body)} bytes")

    manifest, manifest_fetched_at = fetch(EPHEMERIS_MANIFEST_URL)
    chosen, usable_entries = choose_ephemerides(manifest, ephemeris_count)
    saved, skipped = fetch_ephemerides(chosen, work_dir / EPHEMERIS_DIR)
    metadata[EPHEMERIS_DIR] = {
        "manifest": file_metadata(EPHEMERIS_MANIFEST_URL, manifest, manifest_fetched_at)
        | {"usable_entries": usable_entries},
        "selection": "evenly spaced through the manifest, ordered by catalog number",
        "requested": len(chosen),
        "saved": len(saved),
        "skipped": skipped,
        "files": saved,
    }

    # CelesTrak last: its one-download-per-update allowance is only spent once everything
    # else is safely on disk.
    for name, url in CELESTRAK_SOURCES:
        body, fetched_at = fetch(url)
        records = count_records(url, body)
        (work_dir / name).write_bytes(body)
        metadata[name] = file_metadata(url, body, fetched_at) | {"records": records}
        print(f"{name}: {len(body)} bytes, {records} records")

    return metadata


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    today = utc_now().date()
    snapshot_dir: Path = args.out / today.isoformat()
    # Everything is downloaded into a working directory that is renamed only on success, so
    # a failure never leaves a partial snapshot behind.
    work_dir: Path = args.out / f".{today.isoformat()}.partial"
    for path in (snapshot_dir, work_dir):
        if path.exists():
            print(f"error: {path} already exists; refusing to overwrite", file=sys.stderr)
            return 1

    work_dir.mkdir(parents=True)
    try:
        metadata = download_snapshot(work_dir, today, args.ephemeris_count)
    except FetchError as e:
        shutil.rmtree(work_dir)
        print(f"error: {e}", file=sys.stderr)
        print(
            "error: nothing was kept; do not rerun before the next CelesTrak update",
            file=sys.stderr,
        )
        return 1
    except BaseException:
        # Interrupted or an unexpected failure: still leave nothing behind.
        shutil.rmtree(work_dir, ignore_errors=True)
        raise

    (work_dir / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    work_dir.rename(snapshot_dir)
    print(f"wrote {snapshot_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

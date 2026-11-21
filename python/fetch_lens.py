#!/usr/bin/env python3
"""Download the LENS ping files of one dish for the snapshot hours (docs/phase-3-routing.md 6.4).

LENS (github.com/clarkzjw/LENS; Zhao and Pan, MMSys 2024) publishes, per Starlink dish and
hour, pings every 10 ms from the dish to its PoP. Since November 2025 the files are served at
https://lens-starlink.jinwei.me, licensed CC BY-SA 4.0. Default dish: `victoria` (Victoria,
BC, Canada; PoP Seattle; LENS's victoria_active_1).

Requests, all from this one run and none repeated:
  1. the listing of the day's directory, <base>/<dish>/ping/<year>/<month>/<date>/;
  2. each ping file of that listing whose name holds one of the --hours (UTC), at most
     MAX_FILES, at least 2 s apart.
No retries: a request that fails stops the script, which reports what it got and keeps
nothing partial. Generic User-Agent, no personal details, no other host.

Writes data/lens/<date>/<dish>/<file> (not in git: share-alike licence) and
data/lens/<date>/metadata.json (in git): the URL, size, sha256 and the listing's MD5 of every
file, the licence and the fetch time. A file whose MD5 differs from the listing's is an error.

Usage (on the host; needs network access, run by hand):

    python3 python/fetch_lens.py --date 2026-11-18 --hours 21,22,23
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import html.parser
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import snapshot_tools as st

BASE_URL = "https://lens-starlink.jinwei.me"
USER_AGENT = "satellite-network-sim-research"
LICENCE = "CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/)"
SOURCE = "LENS dataset, github.com/clarkzjw/LENS (Zhao and Pan, MMSys 2024)"
MAX_FILES = 6
SECONDS_BETWEEN_REQUESTS = 2.0
TIMEOUT_S = 120


class Listing(html.parser.HTMLParser):
    """Rows of the server's directory listing: (href, md5 from the ETag icon or '')."""

    def __init__(self) -> None:
        super().__init__()
        self.rows: list[list[str]] = []

    def handle_starttag(self, tag, attrs) -> None:
        a = dict(attrs)
        if tag == "a" and a.get("href"):
            self.rows.append([a["href"], ""])
        elif tag == "span" and a.get("data-md5") and self.rows:
            self.rows[-1][1] = a["data-md5"]


def get(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=TIMEOUT_S) as response:
        return response.read()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--date", default="2026-11-18")
    parser.add_argument("--hours", default="21,22,23", help="UTC hours, comma-separated")
    parser.add_argument("--dish", default="victoria")
    parser.add_argument("--out", type=Path, default=st.REPO_ROOT / "data/lens")
    args = parser.parse_args(argv)
    date = datetime.date.fromisoformat(args.date)
    hours = [int(h) for h in args.hours.split(",")]

    directory = f"{BASE_URL}/{args.dish}/ping/{date:%Y}/{date:%m}/{date.isoformat()}/"
    print(f"listing {directory}")
    try:
        page = get(directory).decode("utf-8", errors="replace")
    except (urllib.error.URLError, TimeoutError) as e:
        print(f"error: the listing failed ({e}); stopping, nothing downloaded", file=sys.stderr)
        return 1
    listing = Listing()
    listing.feed(page)
    files = [(h, md5) for h, md5 in listing.rows if not h.endswith("/") and "://" not in h]
    print(f"  {len(files)} files listed: {[h for h, _ in files][:20]}")
    wanted = []
    for href, md5 in files:
        stamp = re.search(r"(\d{4})-?(\d{2})-?(\d{2})[T_ -]?(\d{2})", href)
        if (
            stamp
            and date == datetime.date(*map(int, stamp.groups()[:3]))
            and int(stamp.group(4)) in hours
        ):
            wanted.append((href, md5))
    if not wanted or len(wanted) > MAX_FILES:
        print(
            f"error: {len(wanted)} files match the hours {hours} (expected 1 to {MAX_FILES}); "
            "the layout is not the one described; stopping",
            file=sys.stderr,
        )
        return 1

    target = args.out / date.isoformat() / args.dish
    target.mkdir(parents=True, exist_ok=True)
    records = []
    for href, md5 in wanted:
        time.sleep(SECONDS_BETWEEN_REQUESTS)
        url = directory + href
        print(f"downloading {url}")
        try:
            data = get(url)
        except (urllib.error.URLError, TimeoutError) as e:
            print(f"error: {url} failed ({e}); stopping", file=sys.stderr)
            return 1
        name = Path(href).name
        if md5 and hashlib.md5(data).hexdigest() != md5:
            print(f"error: {name}: MD5 differs from the listing's {md5}", file=sys.stderr)
            return 1
        (target / name).write_bytes(data)
        records.append(
            {
                "file": f"{args.dish}/{name}",
                "url": url,
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "listing_md5": md5,
            }
        )
        print(f"  {len(data)} bytes")
    metadata = {
        "source": SOURCE,
        "licence": LICENCE,
        "dish": args.dish,
        "listing": directory,
        "fetched_utc": datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "files": records,
        "note": "The files are not in git (share-alike licence); python/fetch_lens.py.",
    }
    with (args.out / date.isoformat() / "metadata.json").open("w", encoding="utf-8") as f:
        json.dump(metadata, f, indent=1)
        f.write("\n")
    print(f"wrote {len(records)} files to {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

"""Shared helpers for the Phase 1 validation, comparison and plot scripts.

Reads a snapshot written by fetch_elements.py, runs the C++ `propagate` program, and reads its
CSV back. Independent of the C++ code on purpose: the element-set choice and every time
computation here are reimplemented from the file formats, so a bug in the program is not
repeated here.

Standard library only (no numpy), so every script can import it.
"""

from __future__ import annotations

import csv
import datetime
import json
import re
import socket
import subprocess
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SNAPSHOT = REPO_ROOT / "data" / "starlink" / "2026-11-18"
STANDARD_FILE = "gp.json"
SUPPLEMENTAL_FILE = "supgp.json"
EOP_FILE = "finals2000A.all"

NS_PER_S = 10**9
NS_PER_DAY = 86_400 * NS_PER_S
# Julian date of 1970-01-01T00:00 (the Unix epoch). Times below count from it.
JD_UNIX_EPOCH = 2440587.5
_UNIX_EPOCH_ORDINAL = datetime.date(1970, 1, 1).toordinal()

# YYYY-MM-DDTHH:MM:SS with an optional fraction of 1 to 9 digits: the form of OMM EPOCH
# values and of the program's utc column.
_ISO_UTC = re.compile(r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?$")


def forbid_network() -> None:
    """Makes any attempt to open a network connection in this process raise.

    The scripts read only local files. Skyfield in particular downloads a missing data file
    silently; with this guard such an attempt fails loudly instead.
    """

    def refuse(*_args, **_kwargs):
        raise RuntimeError("network access attempted; these scripts must read local files only")

    socket.socket.connect = refuse  # type: ignore[method-assign]
    socket.socket.connect_ex = refuse  # type: ignore[method-assign]
    socket.create_connection = refuse  # type: ignore[assignment]
    socket.getaddrinfo = refuse  # type: ignore[assignment]


@dataclass(frozen=True)
class UtcInstant:
    """A UTC instant as calendar fields plus exact integer nanoseconds since 1970-01-01.

    Never a single float Julian date: one double near JD 2.46e6 resolves only about 40 us,
    which is 30 cm along a Starlink orbit.
    """

    year: int
    month: int
    day: int
    hour: int
    minute: int
    second_ns: int  # Nanoseconds into the minute, below 60e9 (no leap second).
    unix_ns: int

    @property
    def second(self) -> float:
        """Seconds into the minute; exact for the times the scripts use (multiples of 1/4 s)."""
        return self.second_ns / NS_PER_S

    @property
    def jd_whole(self) -> float:
        """Julian date of 0h UTC of this day (ends in .5), exact in a double."""
        return JD_UNIX_EPOCH + self.unix_ns // NS_PER_DAY

    @property
    def jd_fraction(self) -> float:
        """Fraction of the UTC day, correctly rounded from integer nanoseconds."""
        return (self.unix_ns % NS_PER_DAY) / NS_PER_DAY

    def plus_ns(self, ns: int) -> UtcInstant:
        return utc_from_unix_ns(self.unix_ns + ns)

    def iso(self) -> str:
        whole_s, frac_ns = divmod(self.second_ns, NS_PER_S)
        text = (
            f"{self.year:04d}-{self.month:02d}-{self.day:02d}T"
            f"{self.hour:02d}:{self.minute:02d}:{whole_s:02d}"
        )
        if frac_ns:
            text += "." + f"{frac_ns:09d}".rstrip("0")
        return text


def utc_from_unix_ns(unix_ns: int) -> UtcInstant:
    days, ns_of_day = divmod(unix_ns, NS_PER_DAY)
    date = datetime.date.fromordinal(_UNIX_EPOCH_ORDINAL + days)
    hour, rest = divmod(ns_of_day, 3600 * NS_PER_S)
    minute, second_ns = divmod(rest, 60 * NS_PER_S)
    return UtcInstant(date.year, date.month, date.day, hour, minute, second_ns, unix_ns)


def parse_utc(text: str) -> UtcInstant:
    """Parses YYYY-MM-DDTHH:MM:SS[.f...] (UTC) exactly. Raises ValueError otherwise."""
    match = _ISO_UTC.match(text)
    if not match:
        raise ValueError(f"not a YYYY-MM-DDTHH:MM:SS[.f] time: {text!r}")
    year, month, day, hour, minute, second = (int(g) for g in match.groups()[:6])
    fraction = match.group(7) or ""
    if hour > 23 or minute > 59 or second > 59:
        raise ValueError(f"time of day out of range: {text!r}")
    second_ns = second * NS_PER_S + int(fraction.ljust(9, "0") or "0")
    days = datetime.date(year, month, day).toordinal() - _UNIX_EPOCH_ORDINAL
    unix_ns = days * NS_PER_DAY + (hour * 3600 + minute * 60) * NS_PER_S + second_ns
    return UtcInstant(year, month, day, hour, minute, second_ns, unix_ns)


def seconds_between(later: UtcInstant, earlier: UtcInstant) -> float:
    """later - earlier in seconds, from exact integer nanoseconds."""
    return (later.unix_ns - earlier.unix_ns) / NS_PER_S


@dataclass(frozen=True)
class ChosenElements:
    """The element set a satellite should use, with the CelesTrak product it came from."""

    record: dict
    source: str  # "supplemental" or "standard", as in the program's element_source column.
    epoch: UtcInstant


def _latest_per_satellite(records: list[dict]) -> dict[int, dict]:
    """Latest-epoch record per catalogue number (the first one on a tie)."""
    kept: dict[int, tuple[UtcInstant, dict]] = {}
    for record in records:
        number = int(record["NORAD_CAT_ID"])
        epoch = parse_utc(record["EPOCH"])
        if number not in kept or epoch.unix_ns > kept[number][0].unix_ns:
            kept[number] = (epoch, record)
    return {number: record for number, (_, record) in kept.items()}


def choose_elements(standard: list[dict], supplemental: list[dict]) -> dict[int, ChosenElements]:
    """Per catalogue number: the supplemental set where one exists, else the standard one.

    The rule the program documents (element_selection.hpp), written again from scratch.
    """
    chosen = {
        number: ChosenElements(record, "standard", parse_utc(record["EPOCH"]))
        for number, record in _latest_per_satellite(standard).items()
    }
    for number, record in _latest_per_satellite(supplemental).items():
        chosen[number] = ChosenElements(record, "supplemental", parse_utc(record["EPOCH"]))
    return dict(sorted(chosen.items()))


def load_snapshot_elements(snapshot: Path) -> dict[int, ChosenElements]:
    with (snapshot / STANDARD_FILE).open(encoding="utf-8") as f:
        standard = json.load(f)
    with (snapshot / SUPPLEMENTAL_FILE).open(encoding="utf-8") as f:
        supplemental = json.load(f)
    return choose_elements(standard, supplemental)


def check_snapshot(snapshot: Path) -> None:
    """Raises SystemExit naming the first missing snapshot file."""
    for name in (STANDARD_FILE, SUPPLEMENTAL_FILE, EOP_FILE):
        if not (snapshot / name).is_file():
            raise SystemExit(f"error: {snapshot / name} not found")


def seconds_text(ns: int) -> str:
    """A nanosecond count as a plain decimal number of seconds, as propagate accepts it."""
    whole, frac = divmod(ns, NS_PER_S)
    return f"{whole}.{frac:09d}".rstrip("0").rstrip(".")


def run_propagate(
    exe: Path,
    snapshot: Path,
    start: UtcInstant,
    duration_ns: int,
    step_ns: int,
    ids: list[int],
    out_csv: Path,
) -> str:
    """Runs the program; returns its summary. Raises SystemExit if it fails."""
    command = [
        str(exe),
        "--snapshot",
        str(snapshot),
        "--start",
        start.iso(),
        "--duration-s",
        seconds_text(duration_ns),
        "--step-s",
        seconds_text(step_ns),
        "--out",
        str(out_csv),
    ]
    if ids:
        command += ["--ids", ",".join(str(i) for i in sorted(ids))]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(
            f"error: propagate exited with {result.returncode}\n{result.stdout}{result.stderr}"
        )
    return result.stdout


# Columns of the program's CSV that hold numbers (every column but utc, object_name and
# element_source; norad_cat_id is an integer).
FLOAT_COLUMNS = (
    "element_age_s",
    "teme_x_km",
    "teme_y_km",
    "teme_z_km",
    "teme_vx_km_s",
    "teme_vy_km_s",
    "teme_vz_km_s",
    "ecef_x_km",
    "ecef_y_km",
    "ecef_z_km",
    "geodetic_latitude_deg",
    "geodetic_longitude_deg",
    "geodetic_height_km",
    "j2000_x_km",
    "j2000_y_km",
    "j2000_z_km",
    "j2000_vx_km_s",
    "j2000_vy_km_s",
    "j2000_vz_km_s",
)


def read_propagate_csv(path: Path) -> list[dict]:
    """Rows of a propagate CSV, numbers converted (the text round-trips exactly)."""
    rows = []
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        missing = set((*FLOAT_COLUMNS, "utc", "norad_cat_id", "element_source")) - set(
            reader.fieldnames or ()
        )
        if missing:
            raise SystemExit(f"error: {path} lacks columns {sorted(missing)}")
        for row in reader:
            converted = dict(row)
            converted["norad_cat_id"] = int(row["norad_cat_id"])
            for column in FLOAT_COLUMNS:
                converted[column] = float(row[column])
            rows.append(converted)
    return rows

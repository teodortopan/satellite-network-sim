# Test data for tests/orbit

- `omm_fixture.json`: six real Starlink records copied from the committed snapshot in
  `data/starlink/2026-11-17/` (standard and supplemental GP data).
- `SGP4-VER.TLE` and `tcppver.out`: David Vallado's SGP4 verification cases and his
  published expected output for them (TEME position and velocity at each listed time, made
  with WGS-72 constants and operation mode 'i'). Unmodified, byte for byte, from python-sgp4
  release `2.27`, commit `6e7428df7acc48e8176828289a8c14486a114759`
  (https://github.com/brandon-rhodes/python-sgp4), paths `sgp4/SGP4-VER.TLE` and
  `sgp4/tcppver.out`. python-sgp4 is MIT licensed (copyright Brandon Rhodes); see
  `third_party/sgp4/`.

| File | sha256 |
|---|---|
| `SGP4-VER.TLE` | `d246d1d9d768ace445a38a965713fa9ba52d80fd8a41a0502ff83d7acffe2881` |
| `tcppver.out` | `687bf28dbe52df86e8e60ab5cb4a08d1aa3dbcaf4e63b1f7ab95f044fbe3833b` |

`SGP4-VER.TLE` uses CRLF line endings and `tcppver.out` LF; `.gitattributes` here keeps both
unconverted. The three numbers after each TLE line 2 are the start, stop and step, in minutes
from epoch, of the times listed in `tcppver.out`.

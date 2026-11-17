# Phase 1: Orbits

**Status:** draft.

Sources checked on 2026-11-17:
[CelesTrak GP data formats](https://celestrak.org/NORAD/documentation/gp-data-formats.php),
[CelesTrak supplemental GP data](https://celestrak.org/NORAD/elements/supplemental/),
[CelesTrak astrodynamics code](https://github.com/CelesTrak/fundamentals-of-astrodynamics),
[python-sgp4](https://github.com/brandon-rhodes/python-sgp4).

## 1. Problem

The project question is about latency between cities over a Starlink-like network. Everything
later (links, routing, failures) is built on one thing: knowing where every satellite is, at
any moment, relative to the ground. The repo has a build and CI but no simulator code.

Phase 1 produces that foundation: given a time, return the position of every Starlink
satellite, in a coordinate system that rotates with the Earth so it can be compared with
cities and ground stations. It must also show, with numbers, that those positions are right.

## 2. Background: the terms this spec uses

Read this first. Later sections assume it.

### The data

- **GP data ("general perturbations" data), also called an element set.** A small record per
  satellite, published by the US Space Force and redistributed by CelesTrak. It holds about
  ten numbers describing the orbit at one instant. It is not a list of positions; it is the
  input to a formula that produces positions.
- **Epoch.** The instant an element set describes. Predictions get worse the further you go
  from the epoch, so the age of the data matters.
- **Orbital elements.** The numbers that describe an orbit's size, shape and orientation, and
  where the satellite is along it: mean motion (orbits per day), eccentricity (how
  non-circular), inclination (tilt against the equator), right ascension of the ascending
  node or RAAN (which way the tilted orbit plane faces), argument of perigee (where the
  lowest point is), and mean anomaly (where along the orbit the satellite is).
- **Mean elements.** The elements in GP data are averaged values that only make sense inside
  one specific model, SGP4. Plugging them into a simple textbook orbit formula gives errors
  of many kilometres. This is the most common beginner mistake in this area.
- **BSTAR.** One more number in the element set: how strongly thin upper-atmosphere drag
  slows this satellite.
- **TLE (two-line element set).** The 1960s text format for GP data: two 69-character lines.
  It has room for only five digits of satellite catalog number. Those ran out on 2026-07-11,
  so newly catalogued objects, including new Starlink satellites, are no longer available as
  TLEs.
- **OMM (Orbit Mean-elements Message).** The modern format for the same data, an
  international standard, available as JSON, CSV or XML. No catalog-number limit. This
  project reads OMM in JSON.
- **CelesTrak.** A long-running public site that redistributes GP data with no login.
- **Supplemental GP data (SupGP).** For Starlink, CelesTrak also publishes element sets
  fitted to the trajectory files SpaceX itself publishes. SpaceX knows where its satellites
  are far better than radar tracking does, so these are more accurate than the standard set.
- **Ephemeris.** A table of positions over time, as opposed to an element set plus a formula.

### The propagator

- **Propagation.** Computing where a satellite is at some time, from its element set.
- **SGP4 (Simplified General Perturbations 4).** The standard formula for propagating GP
  data. It is analytical, meaning it computes the position at any time directly instead of
  stepping forward through time. It accounts for the main effects that bend a simple
  elliptical orbit: the Earth's bulge at the equator (called J2), atmospheric drag, and for
  high orbits the Sun and Moon. GP data is produced by fitting SGP4 to tracking data, so
  SGP4 is the only correct way to read it back.
- **Its accuracy.** Roughly a kilometre at the epoch, growing by one to a few kilometres per
  day. That is a property of the method, not a bug to fix.
- **Vallado's implementation.** David Vallado and co-authors published the reference C++
  code for SGP4 in 2006, with a file of test cases and expected outputs. Nearly every SGP4
  library descends from it.

### Coordinate frames

A frame is a choice of origin and axes for writing a position as (x, y, z). The same
satellite has different coordinates in different frames, and mixing frames is the classic
silent error in this field.

- **ECI (Earth-centred inertial).** A family of frames with the origin at the Earth's centre
  and axes fixed relative to the stars. The Earth spins inside it. Orbits are simple here.
- **TEME (True Equator, Mean Equinox).** The particular ECI frame SGP4 outputs. Its z-axis is
  the Earth's rotation axis and its x-axis points at a reference direction on the sky called
  the equinox. It exists only because of how SGP4 was derived.
- **ECEF (Earth-centred, Earth-fixed).** Origin at the Earth's centre, axes glued to the
  Earth and rotating with it. A city has constant ECEF coordinates. This is the frame the
  simulator works in, because the question is about cities.
- **ITRF (International Terrestrial Reference Frame).** The precise, official ECEF frame.
- **PEF (pseudo Earth-fixed).** ECEF before a tiny correction called polar motion.
- **GCRF and J2000.** The precise, official inertial frames.
- **GMST (Greenwich Mean Sidereal Time).** An angle: how far the Earth has turned, measured
  between the equinox direction and the Greenwich meridian. Rotating TEME coordinates about
  the z-axis by GMST gives PEF. This single rotation is the core of TEME to ECEF.
- **Polar motion.** The rotation axis wanders a few metres across the Earth's surface. It
  shifts an ECEF position by up to about 17 m at the surface and 20 m at Starlink's height.
- **Precession and nutation.** Slow wobbles of the Earth's axis against the stars. Needed
  only to convert between TEME and the precise inertial frames.
- **Geodetic coordinates.** Latitude, longitude and height above the ellipsoid.
- **WGS-84.** The standard model of the Earth's shape, a slightly flattened ellipsoid
  (equatorial radius 6378.137 km). GPS and maps use it. SGP4 internally uses the older
  **WGS-72** constants because the element sets were fitted with them. Using WGS-72 inside
  SGP4 and WGS-84 for latitude and longitude is correct, and swapping either is a mistake.

### Time scales

Several definitions of "what time is it" coexist. They differ by under a second to about a
minute, and a satellite moves 7.6 km per second.

- **UTC.** Civil time. Epochs in GP data are in UTC. It is kept near the Earth's rotation by
  occasional leap seconds (the last was at the end of 2016).
- **UT1.** Time defined by the Earth's actual rotation angle. GMST is computed from UT1.
  UT1 stays within 0.9 s of UTC; the difference is measured and published.
- **TAI and TT.** Uniform atomic time scales, with no leap seconds. Needed for precession
  and nutation.
- **Julian date (JD).** A continuous count of days used in astronomy formulas, which avoids
  calendar arithmetic.
- **EOP (Earth orientation parameters).** The published daily measurements of UT1 minus UTC
  and of polar motion.

## 3. Expected outcome

A command-line program and library that, from a saved snapshot of real Starlink GP data,
compute every satellite's position in ECEF and as latitude, longitude and altitude, at any
requested time. Alongside it, a validation report shows how closely the results match
independent references.

What does not exist after Phase 1: links between satellites, ground stations, routing, the
dashboard, the engine API, Python bindings. Those belong to later phases.

### How accurate does this need to be?

Two different questions, which the spec keeps apart:

1. **Is the implementation correct?** Tested against references with tight tolerances. A
   wrong frame or time scale shows up here as an error of kilometres to thousands of
   kilometres, so tight tolerances make mistakes impossible to miss.
2. **How close is the model to reality?** Limited by SGP4 and the data to about a kilometre.
   This is measured and reported, not enforced.

For scale: light travels 300 km in one millisecond, so a 1 km position error changes a link's
latency by 3.3 microseconds. The project question is insensitive to kilometre-level error.
The tight tolerances exist to prove correctness, not because the latency answer needs them.

## 4. Affected systems

New, all inside this repo:

| Path | Contents |
|---|---|
| `third_party/` | Any third-party source copied in, unmodified, with a README stating origin, version and licence |
| `include/satsim/time/`, `src/time/` | UTC instant type, Julian date conversion, GMST |
| `include/satsim/orbit/`, `src/orbit/` | Element-set type, OMM JSON loader, SGP4 wrapper |
| `include/satsim/frames/`, `src/frames/` | TEME to ECEF, ECEF to and from geodetic (WGS-84) |
| `apps/propagate/` | Command-line program: snapshot + time range in, positions out as CSV |
| `tests/` | Unit tests per module, plus the reference test cases |
| `python/` | `fetch_elements.py`, the validation comparison, plots; pinned `requirements.txt` |
| `data/` | One dated Starlink snapshot (standard and supplemental), small test fixtures |
| `docs/phase-1-results.md` | Validation report with numbers and plots |

Changed:

- `docker/Dockerfile`: add Eigen, nlohmann/json, Python 3 with the pinned packages.
- `scripts/ci.sh` and `.github/workflows/ci.yml`: add a `validate` step (runs the Python
  comparison against the C++ output) and a `python-lint` step (ruff).
- `cmake/`: third-party code is built with sanitizers but without our warnings or
  clang-tidy, the same treatment GoogleTest gets.
- `CLAUDE.md`: add the conventions below and the non-obvious domain rules from section 2.

### Conventions fixed by this phase

- Units inside the engine: kilometres, seconds, radians. Degrees only at input and output.
  Names carry the unit: `altitude_km`, `gmst_rad`.
- Vectors are `Eigen::Vector3d`. A position is never passed without its frame being clear
  from the type or the function name (`teme_to_ecef`, `position_ecef_km`).
- A time instant is never a single `double` Julian date (that loses precision to about 40
  microseconds). It is stored as whole days plus a fraction, the convention SGP4 uses.
- A failed propagation (decayed satellite, bad elements) is returned as an error value and
  counted. It never becomes a position.
- The engine does no network access. Only `python/fetch_elements.py` downloads.

## 5. Risks

- **Money / data-critical math.** No money. The critical math is the position chain. One
  hand-traceable example the tests must reproduce: a circular orbit 550 km up has radius
  6378.137 + 550 = 6928.137 km. Speed = sqrt(398600.4418 / 6928.137) = 7.585 km/s. Period =
  2π × sqrt(6928.137³ / 398600.4418) = 5739 s = 95.6 min, which is 15.05 orbits per day.
  Starlink element sets should show mean motions near that value, and propagated positions
  should show that radius and speed.
- **Data integrity.**
  - Stale element sets give confident wrong positions. The loader records each epoch and
    the program reports, and can filter on, element age.
  - Results must be reproducible: all tests and the report run from the saved snapshot,
    never from a live download.
  - Satellites that are still climbing to their shell or are being deorbited are in the
    data. Phase 1 reports them by altitude; Phase 2 decides which count as part of the
    network.
- **Time boundaries.**
  - UTC against UT1: GMST needs UT1 but the data is in UTC. Treating them as equal is an
    error of under 0.9 s of Earth rotation (up to about 450 m at orbit height; tens of
    metres at today's values). Whatever is done here, the report states it and its size.
  - Leap seconds: simulation time is UTC and a simulated window is assumed not to contain a
    leap second. None has been scheduled since 2016. The assumption is written in the code
    and the report.
  - Epoch parsing across day, month and year boundaries, and leap years: unit-tested.
  - Local time zones never appear. Everything is UTC.
- **Security.** No secrets are needed: CelesTrak requires no login. Space-Track (which does)
  is not used in this phase. The repo is public, so nothing credential-like is ever added.
  CelesTrak allows one download per data update (about every two hours) and blocks clients
  that retry on errors, so the fetch script downloads once, stops on any HTTP error, and is
  never run by CI or by tests.
- **Cross-surface.**
  - Licence: the repo is MIT, so AGPL sources must not be copied in. CelesTrak's GitHub
    copy of Vallado's code is AGPL-3.0; the same code inside python-sgp4 is MIT.
  - CI time: propagating about ten thousand satellites over a day under Valgrind would be
    slow. CI uses a small fixture; the full run happens in the `validate` step on an
    optimised build.
  - Floating point: no `-ffast-math`, ever. It breaks the bit-level agreement with the
    reference test cases. Local runs are arm64 and CI is x86-64, so every tolerance
    must hold on both.

## 6. Plan

Four implementation units, each one Opus subagent with a narrow brief, reviewed and verified
by Fable before commit. Units 1 and 2 are independent of each other and can run in parallel;
3 needs both; 4 needs all.

1. **Time and frames.** The UTC instant type, calendar to Julian date, GMST, the TEME to
   ECEF rotation, ECEF to and from geodetic. Tests: published worked examples, known points
   (equator, poles, Greenwich), round trips, the leap-year and boundary cases.
2. **Data.** `python/fetch_elements.py` and the committed snapshot (standard and
   supplemental). The OMM JSON loader, with tests for missing fields and malformed input.
3. **Propagation and program.** SGP4 behind a small `satsim::orbit` interface: build a
   propagator from an element set, ask for TEME position and velocity at an instant, get a
   value or an error. The published SGP4 test cases as parameterized GoogleTest cases. The
   `propagate` program: snapshot, start time, duration and step in; one CSV row per
   satellite per step out, plus a summary (satellites loaded, failed, altitude spread,
   element ages).
4. **Validation and report.** The Python comparison against an independent reference and
   against the supplemental data, wired in as the `validate` CI step. Plots: one satellite's
   ground track, the altitude distribution of the constellation, error against each
   reference, and standard-versus-supplemental difference against element age. Then
   `docs/phase-1-results.md`, and the CLAUDE.md additions.

Each unit lands as its own commit with `scripts/check.sh` green.

## 7. Tests

Existing tests stay green: `tests/core/angles_test.cpp`.

New:

| Test | What it proves | Reference |
|---|---|---|
| `tests/orbit/sgp4_verification_test.cpp` | The propagator reproduces every published test case | Vallado's test file and expected output |
| `tests/orbit/omm_loader_test.cpp` | Real records parse; malformed ones are rejected with an error | Fixture records from CelesTrak |
| `tests/time/utc_time_test.cpp` | Calendar, Julian date and boundary handling | Known dates |
| `tests/time/gmst_test.cpp` | The Earth-rotation angle | Published worked example; rate of 360.9856° per day |
| `tests/frames/teme_ecef_test.cpp` | The frame conversion | Vallado's published TEME to Earth-fixed example |
| `tests/frames/geodetic_test.cpp` | Latitude, longitude, height both ways, including poles | Known points; round trips |
| `tests/orbit/physical_sanity_test.cpp` | Results are physically plausible: radius, speed, period, maximum latitude near the inclination | The hand-traced example in section 5 |
| `python/validate_against_reference.py` | Independent agreement on real Starlink data, in TEME and ECEF | An independent astronomy library |
| `python/compare_supplemental.py` | Measured real-world accuracy (reported, not pass/fail) | CelesTrak supplemental data |

Plus the standing proof that the checks bite: before the phase is called done, a deliberate
frame error (skip the GMST rotation) and a deliberate time error (shift by one second) are
planted temporarily to confirm the validation fails loudly on each.

### Definition of done

- `scripts/check.sh` is green locally and on GitHub, including the new steps.
- Every validation tolerance is met on both arm64 and x86-64.
- `docs/phase-1-results.md` states, with numbers: agreement with each reference, the
  measured gap between standard and supplemental data, every simplification made, and what
  each one costs.
- The `propagate` program runs on the full snapshot with one command and prints its summary.
- You can explain, without notes, why the positions match the references and what would
  break that. The learning list that accompanies this spec covers what that takes.

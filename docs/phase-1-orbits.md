# Phase 1: Orbits

**Status:** implemented 2026-11-18. The results, with every measured number, are in
[phase-1-results.md](phase-1-results.md).

Sources checked on 2026-11-17 and 2026-11-18:
[CelesTrak GP data formats](https://celestrak.org/NORAD/documentation/gp-data-formats.php),
[CelesTrak supplemental GP data](https://celestrak.org/NORAD/elements/supplemental/),
[CelesTrak astrodynamics code](https://github.com/CelesTrak/fundamentals-of-astrodynamics),
[python-sgp4](https://github.com/brandon-rhodes/python-sgp4),
[SpaceX trajectory files](https://api.starlink.com/public-files/ephemerides/MANIFEST.txt),
[IERS Earth-orientation data](https://datacenter.iers.org/data/9/finals2000A.all),
[Skyfield](https://github.com/skyfielders/python-skyfield).

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
- **SpaceX trajectory files.** SpaceX publishes one ephemeris per Starlink satellite: its own
  predicted path for the next two days at 60-second steps, in the J2000 frame, with a stated
  uncertainty at every step. The files are replaced several times a day and there is no
  archive. They are the operator's best estimate, not a measurement of where the satellite
  ended up.

### The propagator

- **Propagation.** Computing where a satellite is at some time, from its element set.
- **SGP4 (Simplified General Perturbations 4).** The standard formula for propagating GP
  data. It is analytical, meaning it computes the position at any time directly instead of
  stepping forward through time. It accounts for the main effects that bend a simple
  elliptical orbit: the Earth's bulge at the equator (called J2), atmospheric drag, and for
  high orbits the Sun and Moon. GP data is produced by fitting SGP4 to tracking data, so
  SGP4 is the only correct way to read it back.
- **Its accuracy.** Roughly a kilometre at the epoch, and worse with age. The usual rule of
  thumb is one to a few kilometres per day. Starlink satellites fly low and thrust often,
  and for them it is much worse: measured against SpaceX's own trajectories, about 0.4 km
  for the first 12 hours, 3 km at 12 to 24 hours and tens of kilometres after one to two
  days (see the results). That is a property of the method, not a bug to fix.
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
- **GCRF and J2000.** The precise, official inertial frames. J2000 has its axes set by the
  Earth's mean equator and equinox on 2000 January 1; it is the frame most orbit data is
  exchanged in. GCRF is its modern replacement and differs from it by about a metre at
  Starlink's height.
- **IAU-76/FK5.** The classical set of models for getting between frames of date and J2000:
  the IAU 1976 precession model and the IAU 1980 nutation model. The newer IAU 2006/2000A
  models differ from it by amounts far below SGP4's error.
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
- **IERS.** The International Earth Rotation and Reference Systems Service, which measures
  and publishes the EOP. Its file `finals2000A.all` has one row per day, including
  predictions for the coming months.

### Checking results

- **Verification.** Showing that the code is a correct implementation of the model.
- **Validation.** Measuring how well the model matches reality.
- **Skyfield.** An astronomy library for Python. Its SGP4 is the same reference code this
  project uses, but its time and frame code is independent of ours.
- **ERFA.** The open-source edition of the IAU's standard astronomy routines (SOFA), with
  independent implementations of the precession and nutation models.

## 3. Expected outcome

A command-line program and library that, from a saved snapshot of real Starlink GP data,
compute every satellite's position in ECEF and as latitude, longitude and altitude, and in
J2000, at any requested time. Alongside it, a report shows how closely the results match
independent references.

The design, in one pass through the chain:

- **Data.** One dated snapshot holds the standard and supplemental element sets, the IERS
  Earth-orientation file and a sample of SpaceX trajectory files, all downloaded in the same
  run. Each satellite uses its supplemental element set, or its standard one where no
  supplemental set exists, and records which. Runs cover one to three days from the
  snapshot.
- **Propagation.** Vallado's reference SGP4, vendored unmodified from python-sgp4 2.27 and
  reached only through this project's own interface. WGS-72 constants, "improved" operation
  mode (the mode only affects orbits with periods over 225 minutes; Starlink's are 87 to 96).
- **Earth-fixed positions.** TEME is rotated by GMST (IAU 1982) evaluated at UT1, then
  corrected for polar motion, giving ITRF. UT1 minus UTC and polar motion come from the
  saved IERS file, interpolated linearly between daily values.
- **J2000 positions.** TEME to J2000 through the classical IAU-76/FK5 chain. The result is
  labelled J2000 (FK5). It is an extra output: nothing in the latency question needs it, but
  SpaceX's trajectory files are in J2000 and it is the frame other tools exchange.
- **Proof.** Three layers, each checking a different part: the published SGP4 test cases
  verify the propagator; Skyfield, ERFA and Vallado's printed examples verify time and
  frames; SpaceX's trajectory files measure the distance from reality.

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
| `third_party/sgp4/` | Vallado's SGP4 source, unmodified, with a README stating origin, version and licence |
| `include/satsim/time/`, `src/time/` | UTC instant type, Julian date conversion, GMST, Earth-orientation table, UT1 and TT |
| `include/satsim/orbit/`, `src/orbit/` | Element-set type, OMM JSON loader, SGP4 wrapper, choice of element set per satellite |
| `include/satsim/frames/`, `src/frames/` | TEME to ECEF with polar motion, TEME to and from J2000, ECEF to and from geodetic (WGS-84) |
| `apps/propagate/` | Command-line program: snapshot + time range in, positions out as CSV |
| `tests/` | Unit tests per module, plus the reference test cases |
| `python/` | `fetch_elements.py`, the comparisons against references, plots; pinned `requirements.txt` |
| `data/` | One dated Starlink snapshot, small test fixtures. The snapshot's SpaceX trajectory files are not committed (about 300 MB, and no stated redistribution terms); their checksums are |
| `docs/phase-1-results.md` | Report with numbers and plots |

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
- A time's scale (UTC, UT1 or TT) is clear from the type or the parameter name, the same
  way a position's frame is.
- The satellite catalog number lives in this project's own types and is never passed into
  the vendored SGP4 code, which stores it in a six-character buffer. Catalogued numbers
  now have six digits, and CelesTrak gives satellites that are not yet catalogued nine-digit
  placeholder numbers.

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
  - The SpaceX trajectory files cannot be downloaded again later, and they are not in the
    repository. The comparison against them is therefore reproducible only on a machine that
    holds the files; its results are committed in the report, with the files' checksums.
  - The trajectory files are forecasts with a stated uncertainty that grows along their two
    days. The comparison reports that uncertainty next to the measured error, so the two are
    not confused.
  - Satellites that are still climbing to their shell or are being deorbited are in the
    data. Phase 1 reports them by altitude; Phase 2 decides which count as part of the
    network.
- **Time boundaries.**
  - UTC against UT1: GMST needs UT1 but the data is in UTC. Treating them as equal is an
    error of under 0.9 s of Earth rotation (up to about 450 m at orbit height). Measured on
    the 2026-11-17 data: 34 to 40 m together with polar motion, 10 to 12 m for polar motion
    alone. Both corrections are applied, so that comparisons against independent references
    are not carrying a built-in disagreement large enough to hide a bug.
  - The Earth-orientation values for the snapshot dates are IERS predictions, not final
    measurements. Their stated uncertainty moves a position by centimetres at most. The
    reference comparison reads the same file, so it is unaffected.
  - A time outside the saved Earth-orientation file is an error, never an extrapolation.
  - Leap seconds: simulation time is UTC and a simulated window is assumed not to contain a
    leap second. None has been scheduled since 2016. The assumption is written in the code
    and the report.
  - Epoch parsing across day, month and year boundaries, and leap years: unit-tested.
  - Local time zones never appear. Everything is UTC.
- **Security.** No secrets are needed: CelesTrak, the IERS and SpaceX's public trajectory
  files require no login. Space-Track (which does) is not used in this phase. The repo is public, so nothing credential-like is ever added.
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

Six implementation units, each one Opus subagent with a narrow brief (the snapshot in unit 3
excepted), reviewed and verified by Fable before commit. Each landed as its own commit with
`scripts/check.sh` green. All six are done.

1. **Time and frames.** The UTC instant type, calendar to Julian date, GMST, the TEME
   to Earth-fixed rotation, ECEF to and from geodetic.
2. **Data.** `python/fetch_elements.py`, a committed snapshot, the OMM JSON loader.
3. **Matched snapshot.** `fetch_elements.py` also saves the IERS file and a fixed sample of
   SpaceX trajectory files, in the same run as the element sets. One fresh snapshot replaces
   the first one.
4. **Earth orientation and J2000.** The Earth-orientation table and its interpolation, UT1
   and TT from UTC, polar motion in TEME to ECEF, and the IAU-76/FK5 chain between TEME and
   J2000. Independent of unit 5.
5. **Propagation.** The vendored SGP4 behind a small `satsim::orbit` interface: build a
   propagator from an element set, ask for TEME position and velocity at an instant, get a
   value or an error. The choice of element set per satellite. The published SGP4 test cases
   as parameterized GoogleTest cases.
6. **Program, validation and report.** The `propagate` program: snapshot, start time,
   duration and step in; one CSV row per satellite per step out, plus a summary (satellites
   loaded, failed, altitude spread, element ages, element-set source). The Python comparison
   against Skyfield, wired in as the `validate` CI step. The comparison against SpaceX's
   trajectories, run by hand. Plots: one satellite's ground track, the altitude distribution
   of the constellation, error against each reference, and error against element age for
   standard and supplemental sets. Then `docs/phase-1-results.md`, and the CLAUDE.md
   additions.

## 7. Tests

Existing tests stay green: `tests/core/angles_test.cpp`.

New:

| Test | What it proves | Reference |
|---|---|---|
| `tests/orbit/sgp4_verification_test.cpp` | The propagator reproduces every published test case | Vallado's test file and expected output |
| `tests/orbit/omm_loader_test.cpp` | Real records parse; malformed ones are rejected with an error | Fixture records from CelesTrak |
| `tests/time/utc_time_test.cpp` | Calendar, Julian date and boundary handling | Known dates |
| `tests/time/gmst_test.cpp` | The Earth-rotation angle | Published worked example; rate of 360.9856° per day |
| `tests/time/earth_orientation_test.cpp` | Earth-orientation values are read and interpolated correctly; out-of-range times are errors | Rows of the IERS file |
| `tests/frames/teme_ecef_test.cpp` | The frame conversion, including UT1 and polar motion | Vallado's published TEME to Earth-fixed example |
| `tests/frames/j2000_test.cpp` | Precession, nutation and the TEME to J2000 chain | Vallado's printed examples; values generated with ERFA |
| `tests/frames/geodetic_test.cpp` | Latitude, longitude, height both ways, including poles | Known points; round trips |
| `tests/orbit/physical_sanity_test.cpp` | Results are physically plausible: radius, speed, period, maximum latitude near the inclination | The hand-traced example in section 5 |
| `tests/orbit/element_selection_test.cpp` | Each satellite gets its supplemental set, or its standard set when there is none | Synthetic input (no real satellite lacks a supplemental set today) |
| `python/validate_against_reference.py` | Independent agreement on real Starlink data, in TEME, ECEF and J2000 | Skyfield, reading the same IERS file |
| `python/compare_spacex.py` | Measured distance from SpaceX's own predicted paths, against element-set age (reported, not pass/fail) | SpaceX trajectory files |

Tolerances, each set just above what its reference can resolve:

| Comparison | Tolerance | Reason |
|---|---|---|
| SGP4 against the published test cases | 1 mm | Same model and inputs; only rounding and print precision differ |
| Frames against Vallado's printed examples | a few cm | His code holds time in one double, which limits his printed values to about 25 mm |
| Frame models against ERFA | near floating-point level | Two implementations of the same published models |
| ECEF against Skyfield | 1 cm | The same model reached by a different route; measured agreement 0.6 mm |
| J2000 against Skyfield | 5 m | Skyfield uses the IAU 2006/2000A models; measured difference 1 to 1.6 m |
| Positions against SpaceX trajectories | none | A measurement. Alarm above 10 km at the epoch, which would mean a bug, not physics |

Plus the standing proof that the checks bite: before the phase is called done, a deliberate
frame error (skip the GMST rotation) and a deliberate time error (shift by one second) are
planted temporarily to confirm the validation fails loudly on each. So are: dropping polar
motion, dropping UT1 minus UTC, flipping a polar-motion sign, and using WGS-84 constants
inside SGP4.

### Definition of done

- `scripts/check.sh` is green locally and on GitHub, including the new steps.
- Every validation tolerance is met on both arm64 and x86-64.
- `docs/phase-1-results.md` states, with numbers: agreement with each reference, the
  measured distance from SpaceX's trajectories for standard and for supplemental element
  sets, every simplification made, and what each one costs.
- The `propagate` program runs on the full snapshot with one command and prints its summary.
- You can explain, without notes, why the positions match the references and what would
  break that. The learning list that accompanies this spec covers what that takes.

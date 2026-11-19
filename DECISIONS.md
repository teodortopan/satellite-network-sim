# Decisions

## Phase 1: Orbits

### 1. Which orbit data to use?

- **Options:** Standard only, supplemental only, supplemental with standard as backup, or live data.
- **Chosen:** Supplemental where it exists, standard as a backup, with runs
  kept to about 12 hours from the element epochs.
- **Why:** The prediction formula (SGP4) gets worse as the element set ages,
  so fresher data matters: supplemental sets are a few hours old, standard
  sets typically half a day and sometimes several days. They also come from
  SpaceX's own plans, so they already know about planned maneuvers. Measured
  against SpaceX's trajectories, the error stays under 0.4 km for 12 hours
  and grows to tens of kilometers after a day. Live data would be ideal, but
  it's not accessible.
- **Tradeoff:** Two paths to maintain and a source label per satellite. The
  simulation depends on SpaceX continuing to publish.

### 2. Where does the SGP4 code come from?

SGP4 (Simplified General Perturbations 4) is the standard formula that turns
an element set plus a time into a satellite's position and velocity. Element
sets are produced to be read by it, so no other formula gives correct results
from them.

- **Options:** Copy in the 2006 Vallado paper's reference code,
  use another library, or write it from scratch using the paper.
- **Chosen:** Vallado's reference code, unmodified, pinned to one version (the copy
  bundled in python-sgp4 2.27)
- **Why:** The orbit numbers only mean something when read by this exact
  formula, so the goal is to reproduce it exactly, not to write similar
  physics. The reference code ships with published test cases that prove a
  copy is faithful. Don't reinvent the wheel.
- **Tradeoff:** The project trusts code it did not write. python-sgp4 is MIT
  licensed, but Vallado's own two files carry no license text.

### 3. Should the small Earth-rotation corrections be applied?

SGP4 gives positions relative to the stars, so placing a satellite over the
ground means rotating by how far the Earth has turned. Two small measured
effects adjust that rotation. UT1 − UTC is the gap between the Earth's actual
rotation (UT1) and clock time (UTC), always under 0.9 s. Polar motion is the
slow wander of the Earth's rotation axis relative to its crust. Both are
measured and published daily by the IERS (International Earth Rotation and
Reference Systems Service).

- **Options:** Ignore both, apply UT1 − UTC only, or apply both.
- **Chosen:** Apply both, from an IERS file saved alongside the snapshot, with
  values interpolated to each simulation time.
- **Why:** Not for latency, which cannot see errors this small. It is so that
  comparisons against independent references are clean. Measured on the
  snapshot, ignoring both shifts positions by 34 to 40 m (polar motion alone
  by 10 to 12 m), enough to hide a real bug. With both applied, the pipeline
  agrees with the Skyfield library to under 1 mm.
- **Tradeoff:** One more input file, a parser and interpolation to test. The
  values for the snapshot dates are IERS predictions, not final measurements,
  which changes positions by centimeters at most.

### 4. Should positions also be converted to the star-fixed J2000 frame?

A frame is a choice of axes for writing down a position. SGP4 outputs in TEME
(True Equator, Mean Equinox), a star-fixed frame used almost only by SGP4.
J2000 is the standard star-fixed frame, with axes set by the Earth's equator
and orbit as they were on January 1, 2000. Getting from TEME to J2000 means
undoing precession and nutation (the slow drift and the small wobble of the
Earth's axis). GCRF (Geocentric Celestial Reference Frame) is the modern
replacement for J2000 and differs from it by about a meter at orbit height.

- **Options:** Skip it, build the classical chain (the IAU-76/FK5 precession
  and nutation models), or build the modern chain (IAU 2006/2000A).
- **Chosen:** The classical chain, as an extra output next to the Earth-fixed
  positions, built after the Earth-fixed pipeline is verified. The result is
  labelled J2000 (FK5).
- **Why:** The latency question does not need it: distances between
  satellites are the same in every frame, and ground geometry uses the
  Earth-fixed frame. But J2000 is the frame the industry exchanges data in,
  SpaceX's own trajectory files use it, and Vallado's worked examples follow
  this chain, so they double as test cases.
- **Tradeoff:** Several more rotations to test and explain. The result sits 1.4
  to 1.6 m from the modern frame (measured against Skyfield), which is far
  below SGP4's kilometer-level error.

### 5. Which references prove the positions are right?

The positions need to be verified (is the model implemented correctly?) and
validated (does the result match reality?).

- **Options:** Vallado paper's published SGP4 test cases, Skyfield, Vallado's
  printed frame examples, the supplemental element sets, or SpaceX's raw
  trajectory files (the predicted paths the supplemental sets are fitted to).
- **Chosen:** Three layers. The published test cases verify the SGP4 code, Skyfield and Vallado's printed examples verify time and
  frames, while SpaceX's trajectory files validate against SpaceX's own
  predicted paths, the closest public thing to reality, reported as a
  measurement and not as pass or fail.
- **Why:** Skyfield runs the same SGP4 code, so it proves little about the
  propagator, but its time and frame code is independent of this project's,
  which is what the frame layer needs. The supplemental sets are the input,
  so checking against them would be checking the input against itself.
- **Tradeoff:** Three test setups. The trajectory files are forecasts that
  cover only the next 48 hours and are replaced several times a day with no
  archive, so they must be downloaded together with the element sets.
  They are SpaceX's best estimate, not ground truth, so their own stated
  uncertainty has to be reported next to the measured error.

### 6. How much tolerance?

- **Options:** One global tolerance such as 1 km, or one tolerance per comparison.
- **Chosen:** One per comparison, each set just above what the reference can
  resolve:
  - SGP4 against the published test cases: 1 mm.
  - Frames against Vallado's printed examples: a few centimeters, because his
    code stores time less precisely and the examples are only good to about
    25 mm.
  - Earth-fixed positions against Skyfield: 1 cm (measured 0.43 mm).
  - J2000 positions against Skyfield: 5 m (measured 1.4 to 1.6 m, because
    Skyfield uses the modern precession and nutation models).
  - Positions against SpaceX's trajectories: no pass or fail. Report the
    error against element-set age, with an alarm above 10 km at the epoch.
- **Why:** A global tolerance wouldn't be able to reliably catch bugs for all comparisons. For some it'd be too large, for others too small. One per comparison ensures all are just right.
- **Tradeoff:** Every number needs a written reason and has to be revisited
  if a reference or a model changes.

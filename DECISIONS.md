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

## Phase 2: Topology

### 1. Link Pattern

- **Options:** Standard 4 links per satellite, 3 links per satellite (used by v2 Starlink satellites), or 6 links per satellite (used by new v3 Starlink satellites).

- **Chosen:** 3 links per satellite as the main model (2 in the same plane, 1 cross plane). Also a 6 link version (2 same plane, 4 cross plane) and the 4 link standard (2 same plane, 2 cross plane) for comparison.

- **Why:** 3 links most closely matches the bulk of Starlink satellites in orbit, while 6 link will continue to grow with time. 4 link is the standard used in the past, so it will be interesting to compare all 3 methods.

- **Tradeoff:** Three link methods to build. SpaceX doesn't publish which satellites link across planes, so that rule is our assumption.

### 2. Finding the Planes

- **Options:** Cluster by RAAN + inclination, use launch groups, drop/keep misfits?

- **Chosen:** Cluster by RAAN, inclination and height, using every satellite's position at the same moment. Drop misfits.

- **Why:** Working satellites sit at the same tilt and height, in tight RAAN groups. The ones that don't are usually raising, lowering or retired. Height matters most: satellites a few km off slide out of place within days.

- **Tradeoff:** Have to tune and justify angles, report misfit counts.

### 3. Which Satellites Count?

- **Options:** All satellites, only satellites with lasers, only satellites at operational altitude

- **Chosen:** Only satellites with lasers at operational altitude

- **Why:** The scope of the project relies on operating satellites and comparing laser link latency/reliability. Satellites not operating and without laser links aren't useful. Also, satellites drifting together form easy lattices, and easy connections.

- **Tradeoff:** Have to filter satellites and count what is left out: about 550 first-generation satellites with no lasers, and about 800 direct-to-cell satellites near 360 km, which could be added later. High latitude coverage is missing until the 70deg and 97deg shells are added.

### 4. Laser Link Limits

Max link length, and minimum clearance above Earth

- **Options:** Geometric cap only (80km clearance, which gives about 4500km max length) or tighter practical caps with a rule for links switching off (accounts for sun, more realistic conditions)

- **Chosen:** Geometric cap as the fixed limit. Tighter caps, sun and lock-on time are settings that can be changed, with a test of how much each one matters.

- **Why:** SpaceX doesn't publish its real limits, so picking one "realistic" cap and calling it fact wouldn't be defensible. Testing a range shows which assumptions actually change the result.

- **Tradeoff:** Extra runs for the tests. Defaults must be justified and reasonable.

### 5. Ground Visibility

Minimum elevation angle for a city to see a satellite, and can it use several or just one?

- **Options:** One angle for all satellites, or the FCC rule by height (10deg below 400km, 20deg from 400 to 500km, 25deg above). Only one satellite per city (realistic), or any visible one (best-case)

- **Chosen:** The FCC rule by height, which means 20deg for almost every satellite today. One satellite per city in the realistic model, any visible one in the best case.

- **Why:** Almost all working satellites now fly between 400 and 500km, where the limit is 20deg. One satellite per city is more realistic.

- **Tradeoff:** per shell masks

### 6. Ground Stations

Does traffic stay in space, or come down at a gateway?

- **Options:** Space only: city -> satellite -> city (best case). Via gateways, fibre and PoP (realistic)

- **Chosen:** Both. The realistic model comes down at a gateway, then fibre to the user's home PoP, then fibre to the destination. We compare it with space only. In some cases fibre is faster.

- **Why:** Real Starlink traffic always goes through the user's home PoP, so space only is the "best case", not how the service works.

- **Tradeoff:** Have to build the more complicated path from space to ground station. The public gateway list only covers the US, so outside the US we assume a gateway at each PoP (45 of them), which is slightly optimistic.

### 7. Cities

What city pairs to study?

- **Options:** Any city pairs really

- **Chosen:** The classics, like NYC -> London, but also some edge cases such as long vertical (Detroit -> Buenos Aires), high latitude (Reykjavik -> Vladivostok) and short distance ones like NYC -> Chicago where fibre should win

- **Why:** No objective reasoning, any city pair works. I chose pairs I'm personally interested in seeing tested, like Detroit-Buenos Aires and Reykjavik-Vladivostok

- **Tradeoff:** Need to make sure edge cases are covered, like short distance pairs, long distance pairs, and high latitude.

### 8. Time Step

How often is the network rebuilt as satellites move in orbit?

- **Options:** 1s (finer, high compute), 15s (how often researchers measured Starlink switching dishes between satellites), 60s (cheaper, may risk missing handoffs)

- **Chosen:** 15s as the base, with a comparison against 1s and 60s

- **Why:** Closest to what Starlink really uses, plus it's not on either end of the extreme. Comparing it to 1s and 60s shows what each step size misses: at 60s every sample has a new satellite, and at 15s about 14% to 30% of the changes seen at 1s are missed. Typical delays barely move.

- **Tradeoff:** 2,880 steps for a 12 hour run. The 15s figure comes from outside measurements, not from SpaceX.

### 9. Proof of Correctness

What proves the topology is right, since SpaceX doesn't publish its data for this

- **Options:** Physical sanity checks, reproducing published results, comparing with Starlink latency.

- **Chosen:** A combination of all 3.

- **Why:** Provides best coverage. The physical checks make sure the links are actually possible. A separate model written in Python has to give the same answer as our program, and that model reproduces Hypatia's published result within 0.5%. Comparing with published Starlink measurements shows if results are plausible.

- **Tradeoff:** None of the three is proof on its own, since no real link map exists. Our 4 link grid is not the same as Hypatia's, so we can't compare with Hypatia directly and need the separate model in between.

## Phase 3: Routing

### 1. Who Plans the Routes, and How Often?

- **Options:** A central planner using predicted orbits, or each satellite working it out from messages with its neighbours. Routes re-planned every 1s, 15s or 60s.

- **Chosen:** A central planner. Routes re-planned every 15s, with 1s and 60s as comparison runs. The planner uses the positions at the start of each interval and holds the route until the next one. A second version that only uses links predicted to stay up for the whole interval is run for comparison.

- **Why:** Orbits are predictable, so one planner can compute consistent routes ahead of time. The evidence points to Starlink doing the same: researchers measured dishes being re-assigned worldwide, in step, every 15s. The re-plan interval is also how stale a route gets, so comparing intervals measures staleness directly.

- **Tradeoff:** Three intervals to run and compare. A path can break in the middle of an interval, and that has to be detected and logged. Satellites working out routes between themselves only matters when something unpredicted happens, so it moves to Phase 4.

### 2. What Does a Route Optimise?

- **Options:** Lowest delay every time, keep the path until it breaks, pick the path that will last longest, or switch only when the gain passes a threshold.

- **Chosen:** Build all four and compare, with lowest delay as the baseline. "Longest-lived" means picking the first and last satellites with the longest time left in view. The threshold is tested over several values.

- **Why:** Most lowest-delay changes gain almost nothing. New York to London changes about 190 times an hour, and the biggest jump is 2ms. Changes still hurt: when a path gets shorter, new packets overtake old ones, and TCP reads that as loss and slows down. One paper more than doubled throughput with longer-lived paths at the same median delay. This is the main experiment of the phase.

- **Tradeoff:** Four strategies to build and a threshold to sweep. No laser link dropped in 12 hours in Phase 2, so a path almost only breaks when its first or last satellite leaves view.

### 3. What Does a Path Change Cost?

- **Options:** Free, counts only, one weighted penalty score, or a full packet simulation.

- **Chosen:** Counts only, reported side by side: total changes, bad changes (the delay drops, which causes reordering), delay increases and breaks, each with its size.

- **Why:** A single score needs a weight that we would have to make up, and that weight would decide the winner. Separate counts keep the comparison honest.

- **Tradeoff:** No single number says which strategy is best. A packet simulation would show the real effect on TCP, but it is a project in itself.

### 4. Serving Satellite Rule

Which satellite does a dish use?

- **Options:** Highest in the sky, longest time left in view, highest among those in view for the whole slot, chosen together with the route, or least loaded.

- **Chosen:** Highest in the sky as the main rule, held for a 15s slot. The next three are comparison runs in the realistic model. The laser route behind the dish can change while the serving satellite stays the same.

- **Why:** Highest is the common baseline: shortest distance up and best signal. Measurements show Starlink prefers high satellites, but not strictly. Choosing together with the route is the only rule that checks if the satellite can reach anything, which is what breaks Reykjavik to Vladivostok.

- **Tradeoff:** Four rules to run. In the best case the route picks these satellites itself, so the rules only apply to the realistic model. Least loaded needs a traffic model, so it is left for later.

### 5. Clocks and Run Length

- **Options:** Keep the 15s step from Phase 2, or go down to 1s. One 12 hour run, or a shorter one.

- **Chosen:** 1s step. Routes re-planned every 15s, serving slot of 15s. One 2 hour window, starting at the same time as Phase 2, with more windows added if results depend on the window.

- **Why:** A 1s step catches path changes a 15s step misses, over a run short enough to stay cheap.

- **Tradeoff:** Hypatia found that even 1s misses changes for 6% of pairs, so every count is a lower bound. Two hours is one sixth of the Phase 2 run.

### 6. What Counts as Reliable?

- **Options:** Any mix of change counts, path lifetime, jump size, time with no path, stretch and backup path delay.

- **Chosen:** All of them: changes per hour, how long a path lasts, jump size, bad changes, time with no path, stretch (delay ÷ (great-circle distance ÷ c)), and the delay of a backup path that shares no satellite with the main one. Every drop is logged with its time, length, city pair, the satellite or link responsible, and a cause.

- **Why:** A steady 40ms and a 40ms that jumps to 140ms are not the same service, and the median alone hides that. Logging the cause of each drop shows if it came from coverage, the serving rule, a laser link, a split network or a missing gateway.

- **Tradeoff:** Many numbers to report. Buildings, terrain and weather are not modelled. In the realistic model the backup path has to share the serving satellite, since there is only one.

### 7. Proof of Correctness

- **Options:** Reproduce Hypatia's path change counts, reproduce the Bariloche to Beijing paper's counts, compare with real dish logs.

- **Chosen:** The first two as proof, each on the paper's own constellation, time step and definition of a change. Real dish logs as a plausibility check if the dates can be matched.

- **Why:** Hypatia gives a median of 4 path changes per 200s. The Bariloche paper gives 185 changes for lowest delay, 103 for keep until broken and 58 for longest-lived, which tests exactly the strategies in decision 2. A published number only proves something under a matching setup.

- **Tradeoff:** Our 4 link grid is not Hypatia's, so the program has to read a link list from a file. The Bariloche paper has no public code and doesn't state every setting, so an exact match may not be possible.

## Phase 4: Failures

### 1. What Fails?

- **Options:** Hand-picked failures (one satellite or laser on a live route, a whole plane, a region, a gateway), a random share of satellites or lasers from 0.1% to 30%, a random rate based on real data, or an attack on the busiest satellites.

- **Chosen:** Hand-picked failures as the main experiment. Also the random sweep, and one attack run. The real rate is only mentioned for context.

- **Why:** Real losses are too rare to show anything: about one satellite a week out of 11,000, so a 2 hour run would see 0.01 failures. Hand-picked failures are the only way to measure recovery on the six study pairs. Random shares and attacks are what the papers use, so results can be compared.

- **Tradeoff:** The scenarios are our choice, not reality's. The random sweep needs many repeated runs.

### 2. How Does a Failure Behave?

- **Options:** Instant and permanent, instant then back after a set time, lasers dropping at random to match SpaceX's 99% uptime, or degraded (less capacity, not a full outage).

- **Chosen:** Instant. Either permanent, or back after 10s or 60s. Random laser drops as an optional extra run.

- **Why:** "Without warning" means sudden. Every study uses instant loss. Degraded needs a traffic model, which we don't have.

- **Tradeoff:** No partial failures. A laser that comes back has to lock on again before it carries anything.

### 3. Who Reacts?

- **Options:** The central planner at its next plan, the planner plus local repair (the satellite next to the failure switches to a backup right away), every satellite told by flooding and working out its own routes, or neighbours swapping distance tables.

- **Chosen:** Build the first three and compare. Distance tables are left out.

- **Why:** The planner alone is the Phase 3 baseline, where a broken route stays down until the next plan. Local repair is the cheapest real improvement. Flooding is the "satellites route among themselves" idea moved here from Phase 3. Distance tables settle slowly after a failure and no satellite paper has numbers for them.

- **Tradeoff:** Three mechanisms to build and test. Nobody outside SpaceX knows which one Starlink uses.

### 4. How Long Do Detection and Reaction Take?

- **Options:** One fixed set of times, or settings with a sweep.

- **Chosen:** Settings with a sweep. Detection at 10ms, 100ms, 1s and 40s. The report to the ground takes as long as the path to the nearest gateway. Passing a message on takes about 1ms per hop, working out new routes 10 to 100ms.

- **Why:** SpaceX publishes none of these, so one number wouldn't be defensible. Same approach as the laser limits in Phase 2. The sweep shows which time actually decides the result.

- **Tradeoff:** More runs. The 1ms and the 10 to 100ms are our own guesses, with no source.

### 5. Time Resolution

- **Options:** Count recovery in whole 1s steps, keep 1s steps for the satellites but work out recovery in continuous time, shorter steps near a failure, or a full event simulator.

- **Chosen:** 1s steps for where the satellites are. Recovery is worked out in continuous time on that step's frozen network.

- **Why:** Recovery takes milliseconds, so whole steps would round it to 0 or 1. Within one second the delays change by microseconds, so freezing the network is accurate. The other two cost far more and add nothing without packets.

- **Tradeoff:** This changes a Phase 3 rule: events between two 1s steps are no longer out of scope, for recovery only. A recovery longer than a second has to carry into the next step.

### 6. Re-pointing Lasers

- **Options:** None (a laser that loses its partner stays idle), freed lasers find a new partner after a loss, also re-point across gaps we can predict, or re-match the whole shell every so often.

- **Chosen:** Freed lasers find a new partner, and the new link works after an acquisition time. 30s as the default, with 2s, 12s and 100s for comparison. No re-pointing as a comparison run.

- **Why:** It's the change that failures actually need. 30s and 2s are a laser maker's numbers (first contact, then every time after). 100s is the US military standard's limit. 12s is what SpaceX says is its fastest, but we couldn't confirm it. Re-matching a whole shell is done for traffic, which we don't model.

- **Tradeoff:** The laser plan is no longer fixed for a run, so the checks have to accept a plan that changes. The default is a vendor's number, not Starlink's. A freed laser can only pair with one facing the right way, so only part of the damage is repaired.

### 7. The Ground

- **Options:** Space only, plus losing a gateway, or plus PoPs, fibre and the core software.

- **Chosen:** Plus losing a gateway. Real US gateways and the ones we assumed at PoPs are reported separately.

- **Why:** 9 of the 11 directions with service use one gateway almost all the time, so it matters. In the US the next gateway is 4 to 16km away. Outside the US we only assumed one per PoP, so the next is 500 to 2,500km away, and losing it mostly tests our own assumption. Nothing public describes the PoPs or the core software well enough to model them.

- **Tradeoff:** The results outside the US say more about our assumption than about Starlink. An outage like July 2025 (2.5 hours, core software) can't be modelled.

### 8. Runs and Measures

- **Options:** The full 2 hour window or short windows around each failure. 30, 100 or 1,000 repeats for the random runs. Any mix of reachability, extra delay, time sent to a dead satellite, loop time, time until stable, and message count.

- **Chosen:** Each hand-picked failure at 10 different times, with 5 minutes of running after it. The runs that only vary one setting use 4 of those times. 30 repeats for each random share, with the seeds written down. All six measures side by side.

- **Why:** What a failure does depends on where the satellites are at that moment, so one time could be a lucky one. We started with 100 repeats and 10 times everywhere, but the full set took too long to run, so we cut it. 30 repeats is still enough to tell 5% damage from 30%. No single score, same as Phase 3.

- **Tradeoff:** Many runs. No packet loss or throughput, since there are no packets.

### 9. Proof of Correctness

- **Options:** Exact checks against a separate Python model, reproducing a published failure study, comparing with real outages.

- **Chosen:** The exact checks as proof. One published number (StarGlider: the backup costs over 20% more delay in 7% of cases) as a comparison with no pass or fail. Real outages for plausibility only.

- **Why:** No published failure study gives both its setup and its numbers, and a published number only proves something under a matching setup. The exact checks can be exact: a flood has to reach each satellite at the shortest path time, and after recovery every satellite's routes have to match the planner's.

- **Tradeoff:** Nothing outside the project confirms the failure numbers. That's weaker than Phases 2 and 3.

## Phase 5: Dashboard

### 1. What Is the Dashboard For?

- **Options:** A free explorer where the visitor clicks around, a guided explainer with fixed stories, or stories that can also be explored freely.

- **Chosen:** An explainer with guided stories that can also be explored freely.

- **Why:** A free explorer shows dots on a globe and nobody learns anything from it. The stories carry the results of Phases 2 to 4, and exploring is how a visitor checks them.

- **Tradeoff:** More work than either one alone. The visitor can only explore what the stories exported.

### 2. Where Does the Data Come From?

- **Options:** The engine compiled to WebAssembly and run in the browser, a server that runs the engine, or playback of runs the engine saved.

- **Chosen:** Playback of saved runs. No engine in the browser, no server.

- **Why:** The page then shows exactly what the checked engine computed, and it can be hosted anywhere for free. A server costs money and has to be kept running. WebAssembly would be a second build of the engine to prove right.

- **Tradeoff:** The visitor can't invent a failure. Only the exported scenarios exist.

- **Changed** on 2026-11-27 by Phase 6, decision 1.

### 3. How Do Satellite Positions Get to the Browser?

- **Options:** Every position at 1 s steps, sampled positions with interpolation in the browser, or SGP4 in the browser through WebAssembly.

- **Chosen:** Sampled positions from `propagate`, interpolated in the browser, with the interpolation error measured and stated. WebAssembly is the fallback if the files are too heavy.

- **Why:** Two hours at 1 s is about 725 MB. Sampled files are tens of megabytes at most, and the error can be measured against the engine.

- **Tradeoff:** Positions between samples are approximations, even if small ones. It's the one thing the browser computes.

### 4. What Draws the Globe?

- **Options:** CesiumJS, deck.gl, three.js.

- **Chosen:** three.js.

- **Why:** It draws 16,000 to 25,000 points and lines without trouble, it's small, and it needs no account or map token. Cesium is heavy and built around map tiles we don't need.

- **Tradeoff:** The globe, the camera and the coastlines are ours to build.

### 5. Which Stories?

- **Options:** (a) the constellation, shells and laser patterns, (b) one route through time under the four strategies, (c) one failure under the three reaction modes side by side, (d) the network falling apart.

- **Chosen:** All four, built in the order c, b, a, d.

- **Why:** The failure story is the one nobody else shows and the hardest, so it goes first. If time runs out, the last ones are the easiest to drop.

- **Tradeoff:** Four stories is a lot of interface for one page.

### 6. How Is Time Shown?

- **Options:** One fixed speed, a speed slider, or a slider plus automatic slow motion around a failure.

- **Chosen:** A speed slider plus automatic slow motion around a failure, clearly marked.

- **Why:** An orbit takes 95 minutes and a flood takes a fifth of a second. No single speed shows both.

- **Tradeoff:** Slow motion can mislead about how fast things are, so it has to be labelled every time.

### 7. Where Does the Dashboard's Data Live?

- **Options:** Committed to git, stored with Git LFS, or built at deploy time.

- **Chosen:** Built at deploy time by an export script. Not in git.

- **Why:** Tens of megabytes of binary files don't belong in the repo, and everything can be regenerated from the committed snapshot.

- **Tradeoff:** A deploy has to build the engine and run the export first, which takes minutes.

### 8. Where Is It Hosted?

- **Options:** GitHub Pages, Cloudflare Pages, Vercel or Netlify.

- **Chosen:** GitHub Pages, deployed from `main` only when I say so. Every data file stays under 25 MiB so Cloudflare's free plan remains an option.

- **Why:** It's free for a public repo and lives next to the code. The 25 MiB limit costs nothing now and keeps the way out open.

- **Tradeoff:** GitHub Pages can't set the headers that threaded WebAssembly needs, so the WebAssembly fallback would be single-threaded there.

### 9. Proof of Correctness

- **Options:** Checking the page by eye, tests of the page's code only, or tests plus a check of the exported data against the engine.

- **Chosen:** Type check, lint, unit tests, a browser test, and a data check that compares every exported number with the engine's CSV files. All of it is one new step of the check command.

- **Why:** A dashboard can look right and be wrong. The data check makes sure every number shown came from the engine, and the browser test makes sure the page actually loads and draws.

- **Tradeoff:** Whether the picture explains things well can't be tested. That stays a judgment.

## Phase 6: Playground

### 1. How Does a Visitor Get a Route Between Any Two Places?

- **Options:** The engine compiled to WebAssembly and run in the browser, a fixed list of city pairs computed ahead of time, or a server that runs the engine.

- **Chosen:** The engine in the browser, next to the saved runs. No server.

- **Why:** A visitor should be able to try their own two places. The check proves the engine in the browser gives the same routes as the tested one, and the site stays a free static page.

- **Tradeoff:** A second build of the engine to keep proven, and a larger CI image. A run made in the browser is the best case only, with three of the four strategies.

### 2. How Does a Visitor Choose the Places?

- **Options:** A search box plus clicking the globe, a search box only, or a short built-in list of cities.

- **Chosen:** A search box over Natural Earth's list of places, plus clicking any point on the globe.

- **Why:** A name is the easiest way in, and a click covers every place the list doesn't have.

- **Tradeoff:** The list is a map maker's selection, not every town. A clicked point has no name.

### 3. Which Colours?

- **Options:** Strict black, white and greys, or greys plus one red for failures.

- **Chosen:** Strict black, white and greys on a black background.

- **Why:** That's the look asked for.

- **Tradeoff:** Lines and dots are told apart by dashes, width and shape, which is harder to read at a glance than colour.

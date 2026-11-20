# Phase 2: Network topology

**Status:** implemented 2026-11-19. The results, with every measured number, are in
[phase-2-results.md](phase-2-results.md).

Sources checked on 2026-11-19:
[FCC order DA 26-36](https://docs.fcc.gov/public/attachments/DA-26-36A1.pdf),
[Starlink constellation altitudes](https://space-safety.starlink.com/docs/space-safety-articles/constellation_altitudes),
[Starlink V3 satellites](https://starlink.com/updates/starlink-version-3-satellites),
[Starlink PoP file](https://geoip.starlinkisp.net/pops.csv) and
[location feed](https://geoip.starlinkisp.net/feed.csv),
[community gateway dataset](https://huggingface.co/datasets/juliensimon/starlink-ground-stations),
[Hypatia](https://github.com/snkas/hypatia),
[Making Sense of Constellations](https://arxiv.org/abs/2307.00402v1),
[A Multifaceted Look at Starlink Performance](https://arxiv.org/pdf/2310.09242),
[Measuring the Satellite Links of a LEO Network](https://www.ece.uvic.ca/~cai/icc24.pdf),
[Starlink in Northern Europe](https://arxiv.org/html/2502.15552v2),
[Dissecting Latency in the Internet's Fiber Infrastructure](https://ar5iv.labs.arxiv.org/html/1811.10737).

## 1. Problem

Phase 1 answers "where is every satellite at this moment". The project question is about
latency between cities, and latency needs a network: which satellites are connected to
which, which satellites a city can reach, and where traffic comes back to the ground.

Phase 2 produces that network as it changes over time. For each time step it builds a
graph whose nodes are satellites, cities, gateways and points of presence, and whose edges
are links with a delay. From the graph it reports the delay between pairs of cities. It
must also show, with numbers, that the network is physically possible and plausible, since
SpaceX does not publish its real link map.

Routing proper (how packets choose paths, what happens when the network changes under
them) is Phase 3. Phase 2 uses one plain shortest-path calculation, only to turn a graph
into a delay that can be checked.

## 2. Background: the terms this spec uses

Phase 1's glossary (`phase-1-orbits.md`, section 2) covers orbits, frames and time. New here:

### The constellation

- **Shell.** A group of satellites at the same height and the same tilt (inclination).
  Starlink has several. A shell's satellites drift together, so their relative layout is
  stable.
- **Orbital plane.** The flat surface an orbit lies in. A shell is made of many planes
  with the same tilt, turned around the Earth's axis by different amounts. Satellites in
  one plane follow each other along the same path.
- **RAAN (right ascension of the ascending node).** The angle that says which way a plane
  is turned around the Earth's axis. Two satellites with the same tilt and RAAN are in the
  same plane.
- **Argument of latitude.** The angle along the orbit from the point where the satellite
  crosses the equator going north. It gives the order of satellites within a plane.
- **Direct-to-cell satellite.** A Starlink satellite that serves ordinary phones. They fly
  lower (about 360 km) than the broadband satellites.

### Links

- **Laser link (inter-satellite link, ISL).** A laser beam carrying data between two
  satellites. Each end uses one laser terminal. A satellite has a fixed number of
  terminals (three on the V2 Mini generation, six on V3, none on the first generation).
- **+Grid.** The link pattern used in most research: each satellite links to the one
  ahead and the one behind in its plane, and to one satellite in the plane on each side.
  Four links per satellite.
- **Line of sight.** Two satellites can link only if the straight line between them does
  not pass through the Earth or the dense lower atmosphere.
- **Elevation angle.** How high above the horizon a satellite appears from a point on the
  ground: 0° on the horizon, 90° straight overhead.
- **Elevation mask.** The lowest elevation at which a ground terminal may use a
  satellite. Below it, buildings, terrain and the long path through the air make the link
  unusable or unlawful.

### The ground

- **User terminal (dish).** The customer's antenna.
- **Gateway.** A ground station with large antennas that connects satellites to the
  ground network.
- **PoP (point of presence).** A site where Starlink's network meets the rest of the
  internet. Each customer is assigned a **home PoP**; all their traffic is carried to it
  before it enters the internet.
- **Bent pipe.** The simplest path: dish up to one satellite, straight down to a gateway.
- **Fibre.** The cables that carry almost all long-distance internet traffic. Light in
  glass travels at about two thirds of its speed in a vacuum, and cables do not follow
  straight lines.

### Measures

- **Delay (one-way latency).** Time for a signal to travel a path, here distance divided
  by speed. Queueing and processing delays are not modelled.
- **RTT (round-trip time).** There and back: twice the one-way delay when both directions
  use the same path.
- **Great-circle distance.** The shortest distance between two points along the Earth's
  surface.
- **Lower bound.** Great-circle distance divided by the speed of light in a vacuum. No
  path outside the Earth can be faster.
- **Stretch.** A path's delay divided by its lower bound.
- **Hypatia.** An open-source simulator of satellite networks (ETH Zurich, 2020) and the
  reference most research compares with.

## 3. What the data looks like

Measured on the 2026-11-18 snapshot. SpaceX lowered the constellation during 2026; its
own altitude page lists the same shells.

Heights are mean heights above the WGS-84 ellipsoid over one orbit.

| Group | Height | Satellites | Role in this phase |
|---|---|---|---|
| 53.2° | 469.5 km | 3,351 | Working shell; the development shell |
| 43° | 487 km | 3,218 | Working shell |
| 97.3° (sun-synchronous) | 477.5 km | 1,446 | Working shell |
| 70° | 485 to 487.5 km | 596 | Working shell |
| 53.2°, 8 km above its shell | 477.5 km | 474 | Excluded: few real planes, drifting across the shell's; may be added later |
| 70° at the old height | 515 to 560 km | 168 | Excluded: being lowered |
| Direct-to-cell | 350 to 375 km | 797 | Excluded: a different service; may be added later |
| First generation, any height | any | 553 | Excluded: no laser terminals |
| Everything else | any | 632 | Excluded: raising, lowering, decaying, or at no shell's tilt |

Two V3 launches (52 satellites, 30.5°, below 350 km) are in the snapshot and still
raising, so the six-laser pattern is a what-if on today's satellites, not a model of V3.

## 4. Design

### 4.1 Modules

| Module | What it holds |
|---|---|
| `orbit/snapshot` | Loading a snapshot directory, shared by all programs |
| `constellation/` | Shells, planes, order within a plane, and why each excluded satellite is excluded |
| `ground/` | Cities, gateways, PoPs, the home-PoP rule, the fibre model |
| `topology/` | Link geometry, the laser patterns, ground links, the per-step graph, the physical checks |
| `network/` | Shortest path over a graph |
| `apps/constellation/` | Program: snapshot in, each satellite's shell, plane and position or exclusion reason out |
| `apps/topology/` | Program: snapshot, ground data and settings in, delay per city pair and time step out |

Later modules may depend on earlier ones, never the reverse. All of them follow
`CLAUDE.md`: kilometres, seconds and radians inside, the unit, frame and time scale in every
name, failures as counted error values.

### 4.2 Shells and planes

- **From propagated positions, not raw elements.** Every satellite is propagated to one
  reference time (the start of the run). Its tilt and RAAN come from position × velocity
  in TEME, and its argument of latitude from its position. Element sets have different
  epochs and plane orientations drift by degrees per day, so raw values are not
  comparable; they are also mean elements, which mean nothing outside SGP4.
- **Mean height** is the average over one orbit of the height above the WGS-84 ellipsoid.
  Tilt and RAAN are likewise averaged over one orbit (32 samples), which removes the
  small wobble of the instantaneous values.
- **A shell** is an inclination range and a height range. A satellite inside both belongs
  to the shell.
- **A plane** is a cluster of a shell's satellites in RAAN: sorted by RAAN, a gap larger
  than the shell's threshold starts a new plane. A plane with fewer satellites than the
  minimum is not a plane.
- **Order.** Planes are numbered by increasing RAAN within a shell. Satellites are
  numbered by increasing argument of latitude within a plane. Ties go to the lower
  catalogue number.
- **Exclusion reasons**, exactly one per excluded satellite, checked in this order:
  propagation failed; launch unknown; no laser terminals (launch designator up to and
  including 2021-044); then, for a satellite in no shell: a named excluded group
  (direct-to-cell height, old height, other height); at a shell's tilt but not its
  height; at no shell's tilt; and last, in a shell but in no plane.
- The shell ranges, gap thresholds and minimum plane size are one configuration with
  documented defaults. The defaults and the evidence for them are in section 5.

### 4.3 Laser links

Positions for all link geometry are in the Earth-fixed frame (ITRF) at the same instant.

- **Line of sight.** The straight segment between two satellites must stay at least
  80 km above a sphere of the WGS-84 equatorial radius (6,378.137 km). The longest
  possible link therefore depends on both heights: about 4,500 km between two satellites
  at 462 km. It is computed, never a fixed number.
- **Neighbours are chosen once**, at the reference time, and kept for the run. At each
  step a link is up only if it has line of sight and passes the configured limits
  (4.5). A link that is down is counted, with the reason.
- **Lasers stay within a shell.** Planes in different shells drift at different rates.
- **In-plane links (all patterns).** Each satellite links to the next and the previous
  satellite in its plane. A plane is a ring, so the last links to the first.
- **Neighbouring planes.** Plane p's neighbour to the east is always plane p+1, and the
  last plane's is the first, however wide the RAAN gap between them. Two planes to the east
  is plane p+2. A shell of one plane has no neighbour; in a shell of two planes each is the
  other's neighbour on both sides.
- **Matching across two planes.** Candidate pairs (one satellite from each plane) are
  taken shortest distance first, at the reference time. A pair is accepted if both
  satellites still have a free laser facing that side, the link has line of sight at the
  reference time, and the link is permanent. Ties go to the lower catalogue numbers.
- **Permanent link.** Each satellite is modelled on a circular orbit of its own mean radius,
  in its own orbit plane, starting from its argument of latitude at the reference time;
  both move at the same rate. The link is permanent if the segment between the two keeps
  line of sight at each of 360 equally spaced points of one orbit. (The widest angle the two
  ever reach has an exact formula; where it settles the answer with 0.1° to spare, the
  program skips the 360 points, with the same result.) Across a RAAN gap too
  wide for any permanent link, no cross-plane link is made and those lasers stay unused
  (`no_free_partner`). This rule is this project's assumption: SpaceX publishes nothing on
  which planes link.
- **The three patterns** differ only in which lasers face where:

| Pattern | In-plane | Cross-plane | Lasers |
|---|---|---|---|
| `three` (main) | ahead, behind | One. Satellites at even positions in their plane face east, odd positions face west. | 3 |
| `plus-grid` | ahead, behind | One east and one west. | 4 |
| `six` | ahead, behind | One east, one west, and one to the plane two to the east and two to the west. | 6 |

  The `three` rule and the `six` layout are this project's assumptions. SpaceX has
  published neither.

### 4.4 Ground links

- **Elevation** is measured from the local horizontal at a point on the WGS-84 ellipsoid,
  at height zero.
- **Mask by height**, from FCC order DA 26-36: 10° for satellites below 400 km, 20° from
  400 to 500 km, 25° above 500 km. The height used is the shell's mean height. An option
  applies the older 5° allowance above 62° latitude; it is off by default.
- **Gateways** use the same mask and may use every visible satellite. Only gateways
  marked operational are used.
- **Cities** depend on the endpoint model (4.6).
- Excluded satellites carry no traffic of any kind.

### 4.5 Limits that can be changed

SpaceX publishes no limits for its laser terminals. These are parameters with defaults,
and the report shows how much each one moves the results.

| Limit | Default | Meaning |
|---|---|---|
| Atmosphere layer | 80 km, fixed | The height a laser beam must clear |
| Range cap | none | A maximum link length below the line-of-sight limit |
| Sun exclusion | off | A link is down while the Sun is within a set angle of the beam, as seen from either end |
| Lock-on time | 0 s | A link that comes up carries nothing for this long. Every link counts as just come up at the first step |
| Fibre factor | 1.98 | Fibre delay relative to light in a vacuum over the great-circle distance |
| Serving-satellite rule | highest elevation | How a city's one satellite is chosen in the realistic model |

### 4.6 Delay and the two endpoint models

- **Delay of a link** is its length divided by the speed of light in a vacuum
  (299,792.458 km/s), for lasers and radio alike. **Fibre** between two ground points is
  great-circle distance × fibre factor ÷ the same speed.
- **Realistic ("Starlink as an ISP").** A Starlink customer in city A talks to a server in
  city B. The path is: A's dish, one serving satellite (the highest above the mask),
  laser links, a satellite in view of a gateway, the gateway, fibre to A's home PoP, fibre
  to B. The gateway and the laser route are those that give the smallest total delay to
  the home PoP. Direction matters: B to A uses B's home PoP. A city with no home PoP has
  no service.
- **Best case ("Starlink as a backbone").** Dishes at both cities, each free to use any
  satellite above the mask, and the smallest delay through space only.
- **Home PoP.** From SpaceX's two files: the PoP that carries the most of the city's
  address blocks. If the city has none, the nearest of the PoPs used in its region, then
  in its country. The level used is reported.

### 4.7 Time

- Run: 12 hours from 2026-11-18 21:15:00 UTC, the limit set in Phase 1, in steps of 15 s
  (2,881 instants). 15 s is the interval at which measurements show Starlink reassigning
  dishes to satellites.
- The ground data is dated 2026-11-19, a day after the element sets.

### 4.8 The `topology` program

Input: snapshot directory, ground-data directory, city file, start, duration, step, link
pattern, endpoint model, the shells to use, the limits of 4.5, and optionally a file of
shell definitions that replaces the built-in ones of section 5.

Options added for Phase 3 (all off by default, which leaves the output unchanged):

| Option | Meaning |
|---|---|
| `--links-file <file>` | Read the laser links from a link file (Phase 3 spec 4.9) instead of planning them |
| `--max-lasers <n>` | With `--links-file`: the most links one satellite may have; default the pattern's count |
| `--links-out <file>` | Write the laser links used, in the same format |
| `--min-elevation-deg <deg>` | One elevation mask in [0, 90) for every site, replacing the FCC masks; for published setups only |
| `--max-ground-range-km <km>` | A site may use any satellite at most this far away, with no elevation test (Hypatia's rule); not with `--min-elevation-deg` |

Neither mask override may be combined with the high-latitude allowance. The summary says when
the links came from a file and when a mask override is on. A link file that is refused, or a
`--links-out` file that cannot be written, exits with status 1, like any other bad input file.

Output, one CSV row per city pair, direction and time step: time, the two cities, pattern,
model, status (a path, or the reason there is none), one-way delay, hops, path length,
great-circle distance, lower bound, stretch, and for the realistic model the serving
satellite, gateway and PoP. A summary gives satellites per shell, exclusions by reason,
links by kind, lasers never used, links down by reason, and failed physical checks.

Exit status: 0 on success, 1 if the run fails or any physical check fails, 2 for invalid
arguments.

## 5. Shell and plane settings

Set on 2026-11-19 from the 2026-11-18 snapshot at 21:15 UTC. The evidence is
`docs/img/phase-2/tables.txt` and the plots next to it (`python/plot_constellation.py`).

| Shell | Tilt | Height | RAAN gap that starts a new plane | Planes | Satellites in planes | Plane size: smallest, median, largest |
|---|---|---|---|---|---|---|
| 53° | 53.16° ± 0.3° | 467.5 to 471 km | 0.4° | 121 | 3,196 | 10, 29, 33 |
| 43° | 43.0° ± 0.3° | 485.5 to 489 km | 1.0° | 93 | 3,187 | 11, 32, 51 |
| 97° | 97.28° ± 0.3° | 476 to 479 km | 0.4° | 63 | 1,426 | 10, 24, 24 |
| 70° | 70.0° ± 0.3° | 483.5 to 489 km | 2.0° | 23 | 586 | 10, 26, 44 |

8,395 satellites in 300 planes; 2,814 excluded.

- **Tilt.** Every shell's satellites lie within 0.01° of its centre, so ± 0.3° is wide
  enough not to matter and narrow enough to keep the shells apart.
- **Height.** Each shell keeps within about 1 km of one height. The ranges are a few
  kilometres wide because a satellite 1 km off slides along the orbit by about 1.2° a day
  against its shell, so its place in a plane does not hold.
- **RAAN gap.** Each value is inside the range over which the number of planes does not
  change: 0.2° to 0.5° for 53° (120 to 121 planes), 0.2° to 2° for 43° (92 to 93), 0.1° to
  0.75° for 97° (63), 0.1° to 3° for 70° (22 to 23).
- **Minimum plane size: 10.** A smaller plane has a gap of at least 36° along the orbit,
  about the longest a laser can bridge.
- **Excluded groups.** Direct-to-cell: 350 to 375 km at any tilt. Old height: 70° at 515 to
  560 km. Other height: 53° at 476 to 479 km.

Three properties of the real planes shape the link rules:

- **Most planes are not full.** Gaps along the orbit of 100° to 186° exist in every
  shell. In-plane links across them have no line of sight and are down.
- **About 130 satellites of the 53° shell are in no plane.** They are 2022 launches at the
  right height, in pairs 0.7° apart between the regular planes, filling every other
  slot. At a 0.4° gap they stay below the minimum plane size.
- **The 70° planes hold two groups 2.5 km apart in height**, sliding past each other at
  about 3° a day. Their order holds for a 12-hour run and not for several days.

## 6. How it is shown to be right

No ground truth exists for the real link map. Three layers, none of them proof alone.

### 6.1 Physically possible (every graph, in the program)

| Check | Rule |
|---|---|
| Line of sight | No laser link passes within 80 km of the surface |
| Range | No laser link is longer than the cap |
| Lasers | No satellite has more links than its pattern allows |
| Symmetry | Every laser link exists at both ends |
| Shells | No laser link joins two shells or touches an excluded satellite |
| Mask | Every ground link is at or above its mask |
| Lower bound | No delay is below great-circle distance ÷ the speed of light |

Any failure is counted and makes the program exit with status 1.

### 6.2 Agrees with an independent model, which agrees with Hypatia (CI, no network)

Hypatia's first Starlink shell, built from Hypatia's own orbital elements: 72 planes of 22
satellites, 53°, 15.19 revolutions per day (about 510 km, although Hypatia labels it
550 km), +Grid, 25° mask, ground stations free to use any visible satellite, shortest path.

The comparison is a chain of two links, because the two +Grids differ. Hypatia links each
satellite to the one with the same position number in the next plane. Its planes are
staggered by half a slot, so half of those links are the longer diagonal. This project links
each satellite to the nearest one in the next plane (4.3), which is a different and shorter
set of links. A same-position grid has no meaning on the real constellation, where planes
hold different numbers of satellites, so the program does not have one.

An independent model in Python (`python/validate_topology.py`) sits between the two. It
shares no code with the program: its positions come from python-sgp4 and Skyfield, its grid
is built from plane and slot numbers, and it has its own line-of-sight test, mask and
shortest path. It can build either grid.

| Comparison | Tolerance | Why |
|---|---|---|
| Program: satellites and laser links | exact: 1,584 and 3,168, 4 per satellite | The pattern is fully determined |
| Program against the model with the nearest grid: status, hops, first and last satellite of every path | exact | Both follow the same rules from independent code |
| Program against the model with the nearest grid: one-way delay | 1e-6 ms | Ten times what Phase 1's position agreement allows over the longest path |
| Model with Hypatia's grid against Hypatia's published round-trip times | within 20% | Hypatia's ground-link limit and Earth model differ slightly from this project's |

Hypatia publishes one pair on this shell: Paris to Luanda, 85 to 117 ms.

### 6.3 Plausible against real Starlink (reported, no pass or fail)

- Paths that papers measured (Oulu to New York: 143 ms median round trip) run in the
  realistic model.
- Published general figures as range checks: about 40 ms median round trip from the dish
  to the ground station, and about 5 ms more to the PoP.
- Any pair whose delay is more than 3 times its lower bound is flagged and explained.

### 6.4 Tests

| Area | Tests |
|---|---|
| Shells and planes | Ideal shells come back exactly; a plane across RAAN 0°; two shells; element sets at different epochs; every exclusion reason; shuffled input |
| Geometry | Line of sight at, above and below the limit; elevation at the horizon, overhead and on the ellipsoid's poles and equator |
| Patterns | Exact link counts on ideal shells; at most 3, 4, 6 lasers; uneven planes; groups of planes joined across a wide RAAN gap; no link across a gap too wide for a permanent link; a link in sight at the start but not for a whole orbit; an in-plane gap too long to bridge |
| Ground | Every loader error; address blocks; the home-PoP rule at every level |
| Shortest path | Hand-worked graphs; no path; equal paths |
| Program | Arguments; a small end-to-end run checked against values computed through the library |

## 7. Out of scope

- Queueing, capacity, congestion and packet-level behaviour.
- Routing protocols and the cost of changing routes (Phase 3). Failures (Phase 4).
- Lasers between shells, and links to excluded satellites.
- Weather, terrain and buildings.

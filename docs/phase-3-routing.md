# Phase 3: Routing

**Status:** specified 2026-11-20. The `routing` program (4.2 to 4.8, 6.1, 6.2) implemented
2026-11-20; the independent model and the reproductions of 6.3 and 6.4 not yet.

Sources checked on 2026-11-20:
[Hypatia](https://github.com/snkas/hypatia) and its
[paper](https://bdebopam.github.io/papers/imc2020-hypatia.pdf),
[A Stability-first Approach to Running TCP over Starlink](https://arxiv.org/html/2408.07460v1),
[A Characterization of Route Variability in LEO Satellite Networks](https://par.nsf.gov/servlets/purl/10438180),
[Making Sense of Constellations](https://arxiv.org/html/2307.00402v1),
[Democratizing LEO Satellite Network Measurement](https://arxiv.org/html/2306.07469v2),
[Delay is Not an Option](https://discovery.ucl.ac.uk/id/eprint/10062262/),
[the LENS dataset](https://github.com/clarkzjw/LENS).

## 1. Problem

Phase 2 builds the network at every time step and reports the lowest delay between two
cities at that instant. That is an ideal: it assumes the route can be recomputed at every
instant with perfect knowledge, and that changing route costs nothing.

Phase 3 follows a route through time. A planner chooses a route, holds it, and re-plans on a
fixed interval. The question is what the traffic between two cities experiences: how often
its route changes, how much the delay jumps when it does, how long it has no route at all,
and how much delay a calmer route costs compared with the ideal.

Failures that nobody predicted (a satellite or a laser lost without warning) are Phase 4.
Phase 3 has no traffic load, no packets and no queues: a route is a path and its delay.

## 2. Background: the terms this spec uses

Phases 1 and 2 define orbits, shells, planes, laser links, masks, gateways and PoPs
(`phase-1-orbits.md` section 2, `phase-2-topology.md` section 2). New here:

- **Route.** The list of nodes traffic passes through, from the sending dish to the end of
  the path.
- **Planner.** The one place that computes routes for the whole network, from predicted
  satellite positions.
- **Re-plan interval.** The time between two plans. Between plans every route is held, so
  the interval is also the oldest a route's information can be.
- **Strategy.** The rule the planner uses to choose a route for one city pair.
- **Serving satellite.** The one satellite a dish talks to (realistic model).
- **Serving slot.** The time a dish keeps its serving satellite: 15 s. Researchers measured
  Starlink re-assigning dishes worldwide every 15 s.
- **Path change.** Two consecutive steps, both with a route, whose routes are not the same
  list of nodes.
- **Bad change.** A path change where the delay goes down. Packets sent on the new route
  overtake packets still on the old one (reordering), and TCP, the protocol most traffic
  uses, reads reordering as loss and slows down.
- **Drop.** A step where the pair has no usable route.
- **Ideal delay.** The lowest delay at one step, recomputed at that step (what Phase 2
  reports). In the realistic model the sending dish may use any satellite it sees for the
  ideal, not only its serving satellite: the ideal is then a true lower bound on every route
  of that step (6.1), which Phase 2's number, held to the highest satellite of that instant,
  is not once a dish keeps a serving satellite for a slot.
- **Backup path.** The lowest-delay route that shares no satellite with the current one.
- **Time left in view.** How long a satellite stays at or above a ground site's mask, from
  now, according to the predicted orbits.

## 3. What Phase 2 already shows

Measured on the Phase 2 runs (2026-11-18 snapshot, pattern `three`), with the limit that
those runs record only a path's first and last satellite and its hop count, so the counts
are lower bounds:

| Pair (best case, 1 s steps) | Changes per hour | A path holds (median) | Largest jump |
|---|---|---|---|
| New York to London | 191 | 21 s | 2.0 ms |
| San Francisco to Tokyo | 178 | 17 s | 2.1 ms |
| Frankfurt to Singapore | 125 | 28 s | 1.3 ms |
| Detroit to Buenos Aires | 68 | 42 s | 16.5 ms (44 ms in the 12-hour run) |

- No laser link went down and came back in 12 hours. A route therefore breaks almost only
  when its first or last satellite leaves view.
- Large jumps appear only where the network is thin: Detroit to Buenos Aires and Reykjavik
  to Vladivostok, both carried by the 70° and 97° shells.
- In the realistic model most pairs use no laser at all (dish, one satellite, gateway,
  fibre), so the strategies matter mostly in the best case and on the high-latitude pair.

## 4. Design

### 4.1 Modules

| Module | What it holds |
|---|---|
| `network/` (extended) | Whether a path is still up in a graph, its delay, a shortest path that avoids given nodes |
| `routing/` (new) | The strategies and the state of one held route. Depends on `network` only: it knows nothing about orbits |
| `topology/` (extended) | A link plan read from and written to a file; time left in view; an elevation-mask override |
| `apps/routing/` (new) | The `routing` program |
| `apps/topology/` (extended) | `--links-out` and `--links-file` |

### 4.2 One graph for both endpoint models

At every step the program builds one graph. Every edge has a delay: length divided by the
speed of light for radio and lasers, `ground::fibre_delay_s` for fibre.

- **Satellites,** with an undirected edge for every planned laser link that is up
  (`topology::LinkStateEvaluator`, unchanged).
- **A sending dish per city.** Best case: an edge to every satellite at or above the mask.
  Realistic: an edge to the city's serving satellite only (4.4).
- **Best case: a receiving dish per city,** with an edge from every satellite at or above
  the mask.
- **Realistic: a node per operational gateway,** with an edge from every satellite the
  gateway sees, and **an end node per direction,** with an edge from every gateway whose
  delay is the fibre from that gateway to the sender's home PoP plus the fibre from the home
  PoP to the destination city.

No edge enters a sending dish or leaves a receiving dish or an end node, so no route passes
through a city. A direction's route runs from its sending dish to its receiving dish (best
case) or its end node (realistic). A direction whose sender has no home PoP has no end node
and its status is `no_service` at every step, as in Phase 2.

With this graph the realistic model is an ordinary shortest path, so every strategy works on
both models unchanged. It must give Phase 2's numbers again (6.2).

Details:

- Nodes are numbered satellites first, then for each city its sending dish and its receiving
  dish, then (realistic) the gateways, the end nodes and the serving dishes
  (`apps/routing/graph.hpp`). The best case is numbered exactly as the `topology` program's
  graph, so the two find the same paths with the same ties.
- In the realistic model each sending city also keeps a dish with an edge to every
  satellite it sees. Only the ideal (2) and the `with-route` rule (4.4) start from it; the
  routes start from the serving dish, whose one edge goes to the serving satellite while the
  city sees it.
- The end node's edge from a gateway carries `fibre_delay_s` from the gateway to the home PoP
  plus `fibre_delay_s` from the home PoP to the destination, added in that order, as Phase 2
  adds them.
- `--hop-delay-ms` is added to every laser edge and every radio edge (dish to satellite,
  satellite to receiving dish, satellite to gateway), never to fibre; the ideal includes it.

### 4.3 Time and the planner

- Steps: `start + k × step`. The main runs use a 1 s step.
- **Re-plan instants:** the steps where `k × step` is a multiple of the re-plan interval
  (15 s; 1 s and 60 s as comparisons). The interval must be a multiple of the step.
- **Slot starts:** the steps where `k × step` is a multiple of the serving slot (15 s).
- At a re-plan instant each direction's strategy looks at the **planning graph** and either
  keeps the held route or replaces it. At every other step the route is held unchanged.
- Two planners:
  - `start` (main): the planning graph is the graph of the re-plan step.
  - `whole-interval` (comparison): the planning graph has only the edges that exist at every
    step from this re-plan instant up to, not including, the next one. Edge delays are those
    of the re-plan step.
- At every step, the held route is checked against that step's graph. If every edge exists
  the status is `ok` and the delay is the sum of that step's edge delays. If not, the step
  is a drop (4.6) and stays one until a later plan gives a route.
- Step 0 is always a re-plan instant and a slot start. At a step that is both, the dishes
  pick their serving satellites first and the plan is made with them.
- `whole-interval` near a slot boundary: a serving dish's edge is tested only at the steps of
  the interval that lie in the current slot; which satellite the dish takes in the next slot
  is not predicted.
- Predictions run past the run's last step when they need to (the last plan's interval, the
  time left in view): those steps are propagated like any other, and the Earth-orientation
  file must cover them.

### 4.4 Serving satellite (realistic model)

At a slot start the dish picks its serving satellite among those at or above its mask and
keeps it for the slot. If the satellite falls below the mask inside the slot, the dish has
no uplink until the next slot.

| Rule | Picks |
|---|---|
| `highest-elevation` (main) | The highest elevation; ties to the lower catalogue number (Phase 2's rule) |
| `longest-in-view` | The longest time left in view; ties to the higher elevation, then the lower catalogue number |
| `highest-whole-slot` | The highest elevation among the satellites that stay at or above the mask at every step of the slot; if there is none, `highest-elevation` |
| `with-route` | The first satellite of the route the strategy would choose if the dish could use any visible satellite at the slot start |

Time left in view is predicted by stepping the orbit forward at the run's step until the
satellite is below the mask, up to a horizon of 900 s. A satellite still in view at the
horizon counts as 900 s. Precisely: with positions at steps k = 0, 1, ... from now, and k the
first step at which the dish may not use the satellite (the same test as the dish's ground
links, so under `--max-ground-range-km` the distance; a failed propagation also ends the
view), the time left is k × step, capped at 900 s. Only the steps before the horizon are
looked at (k × step < 900 s). A satellite not in view now has 0 s; one in view now and gone
at the next step has one step. The caller supplies the future positions
(`topology::time_left_in_view_s`, or `GroundVisibility::times_left_in_view_s` for every
satellite a site sees); this code does not propagate.

The serving slot applies to the realistic model only. In the best case the dish may use any
visible satellite, and the route chooses.

Details:

- A dish that sees no satellite at its slot start has no serving satellite until the next
  slot start; its steps are `no_coverage`.
- `with-route`: the route depends on the strategy and the destination, so under this rule each
  direction and strategy has a dish of its own. At the slot start the strategy plans, from
  its own held route, on that step's graph with the dish free to use any satellite it sees;
  this trial plan does not replace the held route. The serving satellite is the first
  satellite of the route it finds; if it finds none, the highest-elevation one.
- `highest-whole-slot` reads the time left in view with the slot as the horizon: a satellite
  qualifies when it may be used at every step of the slot.

### 4.5 Strategies

Each direction keeps one held route. At a re-plan instant, with the planning graph:

| Strategy | Rule |
|---|---|
| `lowest-delay` | Take the shortest path. |
| `keep-until-broken` | If the held route is up in the planning graph, keep it. Otherwise take the shortest path. |
| `longest-lived` | If the held route's access satellites are still in view and the route is up, keep it. Otherwise choose the access satellites with the longest time left in view (below) and take the shortest path through them. |
| `threshold` | Find the shortest path. Keep the held route if it is up and the shortest path is not better by more than the threshold. Otherwise take the shortest path. |

- "Shortest path" is `network::shortest_paths` with its fixed tie rule (lowest delay, then
  fewest hops, then lowest-numbered predecessor), so a run is deterministic.
- "Up" means every edge of the route exists in the planning graph.
- **Threshold** is either a delay in milliseconds or a fraction of the held route's delay.
  Values run: 0.5, 1, 2 and 5 ms; 1%, 2%, 5% and 10%.
- **Access satellites** are a route's first and last satellite: the ones with a ground link.
- **Longest-lived, best case.** This follows the "Tenacious" algorithm of the Bariloche
  paper. At each city take two candidates: among the satellites it sees, the one with the
  longest time left in view that is moving north, and the one that is moving south. A
  satellite's two directions of travel form two meshes that are poorly joined, so one
  candidate of each kind keeps a short route possible. Of the up to four pairs, take the
  lowest-delay pair that lasers join. The route is the shortest path through that pair.
- **Longest-lived, realistic.** The first satellite is the serving satellite (4.4). The
  gateway is the one the shortest path uses at this instant, so fibre is never traded for
  lifetime. The last satellite is chosen among the satellites that gateway sees, with the
  same two candidates and the same rule.
- The paper computes each route at three quarters of the way through the pair's time in
  view. Here the route is computed on the planning graph, like every other strategy. This
  changes delays slightly and does not change when access satellites are switched.

If no route exists at a re-plan instant the direction has none until the next plan.

Details of `longest-lived`:

- A satellite moves north when its Earth-fixed z is larger at the next step than at this one;
  otherwise (and when either position is missing) it moves south.
- "Longest time left" ties go to the higher elevation, then the lower catalogue number. Among
  equal-delay pairs the north head wins, then the north tail.
- Candidates are taken only among the satellites whose ground edge is in the planning graph
  (with `whole-interval`, those the site sees for the whole interval).
- Realistic: with no shortest path at this instant there is no gateway, no candidate and no
  route (`no_gateway`).

### 4.6 Drops and their causes

A step with no usable route is a drop. Its cause is the first of these that applies:

| Cause | When | Responsible |
|---|---|---|
| `no_service` | Realistic: the sender has no home PoP | none |
| `no_coverage` | The sender (best case: either city) sees no satellite at or above its mask | the city |
| `serving_left_view` | Realistic: the serving satellite fell below the mask inside its slot | the satellite |
| `ground_link_lost` | A ground edge of the held route is gone (first or last satellite, or the gateway's satellite, left view) | the satellite and the site |
| `laser_link_down` | A laser edge of the held route is down | the link, with `LinkDownReason` |
| `no_gateway` | Realistic: at the last plan, no gateway could be reached from the serving satellite | the serving satellite |
| `no_path` | Best case: at the last plan, no chain of lasers joined the two cities | none |

Consecutive dropped steps of one direction and strategy with the same cause form one drop
event, with a start time and a length.

Details:

- The first three causes are tested at the step itself. Then, if a route is held,
  `ground_link_lost` or `laser_link_down` for its first missing edge, a ground edge before a
  laser edge (the first in route order of each). A held route whose dish edge is gone because
  a new slot gave the dish another satellite is `ground_link_lost`, naming the old satellite.
- If no route is held, the cause is the one the last plan's step had: `no_gateway` or
  `no_path` as above, or `no_service`, `no_coverage` or `serving_left_view` when that is why
  the plan found nothing.
- Responsible: for `no_coverage` the city that sees nothing (the sender first); for
  `serving_left_view` the satellite and the sending city; for `ground_link_lost` the
  satellite and the city or gateway of the lost edge; for `laser_link_down` the link and its
  `LinkDownReason`; for `no_gateway` the serving satellite. A drop event names what was
  responsible at its first step; a change of responsible satellite or link with the same
  cause does not start a new event.

### 4.7 Measures

Per direction and strategy, over the run:

- **Path changes** per hour, split into bad changes (delay down), delay increases, and
  breaks (an `ok` step followed by a drop). The size of a change is the new route's delay at
  its first step minus the old route's delay at its last step.
- **Route lifetime:** how long one route is used without a change or a drop; median and
  distribution.
- **Time with no route,** by cause.
- **Delay and stretch** (delay ÷ (great-circle distance ÷ c)), and **extra delay against the
  ideal** at the same step.
- **Backup path delay,** computed at each re-plan instant: the shortest path in the planning
  graph with the held route's satellites removed. In the realistic model the serving
  satellite is not removed, because the dish has only one.

Details: rates are per hour of the run, (steps − 1) × step. A route's lifetime is a run of
consecutive `ok` steps with the same route; runs cut by the start or the end of the run are
counted, as in Phase 2. The extra delay is taken at `ok` steps. The backup is computed for the
route held after the plan, its delay and its delay minus the held route's are both in the
planning graph, and re-plans with no backup are counted.

### 4.8 The `routing` program

Input: the snapshot, the ground data, the city file, start, duration, step, link pattern or
a link file, endpoint model, the limits of Phase 2, and:

| Option | Values |
|---|---|
| `--strategies` | A list: `lowest-delay`, `keep-until-broken`, `longest-lived`, `threshold:<n>ms`, `threshold:<n>%` |
| `--replan-s` | Re-plan interval, a multiple of the step |
| `--planner` | `start`, `whole-interval` |
| `--serving` | The rules of 4.4 |
| `--serving-slot-s` | Default 15 |
| `--backup` | `on`, `off` |
| `--min-elevation-deg` | Replaces the FCC masks with one angle; for published setups only |
| `--max-ground-range-km` | Replaces the masks with a longest dish-to-satellite distance (Hypatia's rule); for published setups only |
| `--hop-delay-ms` | A fixed delay added to every radio and laser edge (the Bariloche paper adds processing time per hop); default 0 |
| `--max-lasers` | The most links one satellite may have in a link file; default: the pattern's count |
| `--links-file`, `--links-out` | Read the laser links from a file instead of planning them; write the links used |
| `--directions` | `both` (default), or `forward`: only each pair's from-to (Hypatia's one direction per pair) |
| `--drops-out` | The drops CSV; not written without it |

One run steps through time once and evaluates every listed strategy, since propagating the
satellites is the expensive part.

Output:

- **Routes CSV,** one row per step, direction and strategy: time, cities, role, pattern,
  model, strategy, status, drop cause, delay, ideal delay, laser hops, the full route as
  catalogue numbers, serving satellite, gateway, home PoP, whether a plan was made at this
  step, and the backup delay at re-plan instants.
- **Drops CSV,** one row per drop event: start, length, cities, strategy, cause, the
  satellite, link or site responsible.
- **Summary:** the Phase 2 counts, the measures of 4.7, and the checks of 6.1.

Exit status: 0, 1 if any check of 6.1 fails, 2 for bad arguments or inputs (also for output
that cannot be written).

Details: `--replan-s` and `--serving-slot-s` must be whole multiples of the step, the default
slot of 15 s too when it is not given. The routes CSV (`apps/routing/run.hpp`) has a `status`
of `ok` or `drop`; the route, its laser hops, first and last satellite and gateway are written
on `ok` rows, the route as its satellites' catalogue numbers separated by spaces; `plan` says
what the plan did (`kept`, `replaced` or `none`) at a re-plan instant and is empty otherwise.
Rows are ordered by time, then direction, then strategy; drop events by start, then
direction, then strategy.

### 4.9 Link files

A link file lists laser links as pairs of catalogue numbers. Reading one gives a
`topology::LinkPlan` that passes the same checks as a planned one: both ends are satellites
in planes, no link appears twice, no satellite has a link to itself, and no satellite has
more links than `--max-lasers` (default: the pattern's count). `--links-out` writes the plan
in the same format; written then read gives an identical plan.

The format (`topology/link_file.hpp`):

- Plain text, one line per link, fields separated by commas, no quoting, no spaces, no empty
  lines. A line starting with `#` is a comment and may appear anywhere.
- The first other line is the header: `norad_cat_id_a,norad_cat_id_b`, or
  `norad_cat_id_a,norad_cat_id_b,side_a,side_b` when the laser side used at each end is
  given. Every link line then has two or four fields.
- A side is `ahead`, `behind`, `east`, `west`, `second_east` or `second_west`, and the two
  sides of a link are `ahead` and `behind`, `east` and `west`, or `second_east` and
  `second_west`, in that order.
- `--links-out` always writes the four columns, after two comment lines.

Reading refuses the whole file, naming the line, for: a malformed line or header, a
catalogue number that is not in the run or is excluded, a link from a satellite to itself, a
link given twice (in either order), a satellite with more links than the limit, and a file
that is missing or has no link. A file is someone else's link set, so the planner's rules
(4.3 of Phase 2) are not applied: a link may join any two satellites in planes, in any
planes or shells, and the physical checks then accept links between shells. Without side
columns, a link inside one plane is labelled ahead to behind and any other east to west;
the labels change nothing but the summary's counts by kind. Each satellite keeps the
pattern's lasers, each taken by the first link that uses its side; a link end with no free
laser on its side gets one more laser. Because the sides are written, a planned plan written
and read back with the same pattern is equal to the original in every field, so the link
states and every result are the same.

## 5. Runs

Window: 2026-11-18 21:15:00 to 23:15:00 UTC, 1 s steps (7,201 instants), the six study
pairs of Phase 2 in both directions, ground data of 2026-11-19.

| Group | Varies | Fixed |
|---|---|---|
| Main | All strategies, both models | 15 s re-plan, `start` planner, `three`, `highest-elevation` |
| Interval | Re-plan 1 s and 60 s, both models | `lowest-delay` and `keep-until-broken` |
| Planner | `whole-interval`, both models | `lowest-delay`, 15 s |
| Serving | The three other rules, realistic | All strategies, 15 s |
| Pattern | `plus-grid` and `six`, both models | `lowest-delay` and `keep-until-broken`, 15 s |

## 6. How it is shown to be right

### 6.1 Checks in the program (every step)

Counted in the summary; any failure gives exit status 1.

- A route reported `ok` has every edge in that step's graph.
- No strategy's delay is below the ideal delay of the same step.
- No delay is below great-circle distance ÷ c.
- Every dropped step has a cause.
- A route starts at the sending dish and ends at the receiving dish or the end node, with no
  node twice.
- Phase 2's physical checks on the links (`run_physical_checks`).

The first check is made against the step's link states and ground links themselves, not the
graph built from them, so that a graph-building error shows. An `ok` step with no ideal counts
as below the ideal. The route's shape is checked at every step that holds a route.

### 6.2 Agrees with Phase 2

`lowest-delay`, re-planned every step with a serving slot of one step, must equal the
`topology` program on the same steps: status, delay to 1e-9 ms, laser hops, first and last
satellite, serving satellite and gateway. Both models, real snapshot.

### 6.3 Agrees with an independent model, which reproduces published results

As in Phase 2, a Python model written separately from the C++ must give the same routes and
change counts as the program on the same inputs, before either is compared with a paper.

- **Hypatia, path changes** (its Figure 8a). The setup, read from Hypatia's code:
  - Shell: 72 planes of 22, 53°, 15.19 revolutions per day, planes 5° apart, satellites
    16.36° apart, every odd plane shifted by half a slot.
  - Links: satellite j of plane i to satellite j + 1 of the same plane and to satellite j of
    plane i + 1, wrapping from the last plane to the first. 3,168 links, read from a link
    file.
  - Ground: a city may use any satellite within 1,089.686 km (`--max-ground-range-km`).
    Hypatia tests distance, not elevation.
  - Cities: Hypatia's 100 cities (MIT licence), all 4,950 pairs, one direction each.
  - Time: 200 s at 100 ms steps (2,000 instants), `lowest-delay` re-planned every step, best
    case.
  - Counting: Hypatia's rule. A change is any difference in the list of nodes between two
    consecutive steps; losing the route counts as one change and regaining it as another.
  - Published: a median of 4 path changes per pair, and 10% of pairs with 7 or more.
  - Known difference: Hypatia places satellites with the pyephem library, this project with
    Vallado's SGP4 (Phase 2 measured the effect on delay as under 0.5%).
- **Bariloche to Beijing** (the Stability-first paper, section IV-B). The setup, as far as
  the paper and its 2022 companion state it:
  - Shell: 72 planes of 22, 53°, 550 km. Plane o is at RAAN o × 5°; its satellite i is at
    o × 8.864° + i × 16.36° along the orbit (phase offset 39).
  - Links: ahead and behind in the plane, and the same index in the next plane; from the
    last plane to the first the index shifts by 39. Read from a link file.
  - Ground: one mask of 40° (`--min-elevation-deg`), one access satellite at each end.
  - Delay: propagation plus 1.012 ms per hop (`--hop-delay-ms`).
  - Time: one orbit (5,739 s) at 1 s steps, re-planned every step, best case.
  - Strategies: the paper's Dijkstra, Stubborn and Tenacious are `lowest-delay`,
    `keep-until-broken` and `longest-lived`.
  - Published: 185 route changes for lowest delay (101 bad), 103 for keep until broken (61
    bad), 58 for longest-lived (29 bad); median delay just under 89 ms; great-circle distance
    19,350 km.
  - Not stated in the paper: the cities' coordinates, the start time, the exact counting
    rule. The paper moves satellites on ideal circles; this project uses SGP4 on element sets
    generated for the same shell. No code is public.

Tolerances: the program and the independent model agree exactly on routes and counts.
Against Hypatia the median must match exactly and the 90th percentile within 1. Against the
Bariloche paper the counts are reported with their difference and no pass or fail, because
of the unstated settings; a difference above 10% is investigated and explained, never tuned
away.

A short version of the program-against-model comparison runs in CI as `validate-routing`,
with no network access. The two full reproductions are run by hand and reported.

### 6.4 Plausible against real Starlink (reported, no pass or fail)

The LENS dataset publishes, per dish and hour, pings every 10 ms from the dish to its PoP,
including the hours of this project's snapshot. For a dish whose city and PoP are known, the
realistic model's delay from that city to its PoP over the same two hours is compared with
the measured one: the lowest delay, the size of the jumps, and whether the jumps fall on a
15 s grid. The dish's position is known only to its city, and the model has no queues or
processing, so the model is expected to be a lower bound and the comparison is a plausibility
check. The files are not kept in git (their licence is share-alike); `metadata.json` holds
their sha256.

### 6.5 Tests

- `network`: path up, path delay, shortest path avoiding nodes, on hand-worked graphs.
- `routing`: each strategy on small graphs that change over time, every expected route
  worked out by hand; threshold at, just under and just over the limit.
- `topology`: link file round trip and every refusal; time left in view on a synthetic shell
  where pass lengths are known; the mask override.
- `routing_app`: the graph of 4.2 on a synthetic shell; serving slots and each serving rule;
  both planners; every drop cause; the checks of 6.1 each made to fire.

## 7. Out of scope

- Unpredicted failures and satellites computing routes among themselves (Phase 4).
- Re-pointing lasers during a run (Phase 4).
- Traffic load, queues, packets and TCP itself. Bad changes are counted; their effect on
  throughput is not simulated.
- Buildings, terrain and weather between a dish and a satellite.
- Events between two 1 s steps.

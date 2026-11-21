# Phase 4: Failures

**Status:** specified 2026-11-21. Not implemented.

Sources opened on 2026-11-21:
[SDA Optical Communications Terminal Standard v3.2.0](https://www.sda.mil/wp-content/uploads/2025/04/SDA-OCT-Standard-v3.2.0_FINAL.pdf),
[a survey of optical inter-satellite links, with vendor acquisition times](https://arxiv.org/html/2301.05285),
[a model of acquisition time in low orbit](https://arxiv.org/html/2511.16063),
[Network topology design at 27,000 km/hour](https://bdebopam.github.io/papers/conext19_LEO_topology.pdf),
[a study that sweeps laser acquisition time](https://arxiv.org/html/2601.21914),
[StarGlider](https://arxiv.org/html/2401.11490),
[ThousandEyes on the Starlink outage of 2025-07-24](https://www.thousandeyes.com/blog/starlink-outage-analysis-july-24-2025),
[A First Look at Starlink Performance](https://arxiv.org/html/2307.06863),
[dish-level outages measured](https://arxiv.org/html/2508.13402),
[percolation thresholds](https://arxiv.org/pdf/0901.1370),
[RFC 2328 (OSPF)](https://www.rfc-editor.org/rfc/rfc2328.txt),
[RFC 5880 (BFD)](https://www.rfc-editor.org/rfc/rfc5880.txt).

Not opened, so not relied on: SpaceX's own laser figures (its 2024 SPIE paper sits behind a
bot wall). The 12 s in 4.8 comes from second-hand reports of that paper and is marked as such.

## 1. Problem

Phase 3 follows a route through time when everything goes as predicted. Its planner knows the
orbits, so nothing surprises it, and a route only breaks because the plan was made on one
instant's positions.

Phase 4 breaks things without warning. A satellite, a laser link or a gateway stops working at
some instant, and nobody was told in advance. The question is what the traffic between two
cities experiences from that instant on: how long it is sent towards something dead, whether
it goes round in circles, how long until it has a working route again, how much slower that
route is, and how those answers depend on who reacts and how fast.

Phase 4 still has no traffic load, no packets and no queues. A route is a path and its delay,
as in Phase 3. What is new is that the network's true state and what each part of it
*believes* are no longer the same thing.

## 2. Background: the terms this spec uses

Earlier phases define orbits, shells, planes, laser links, gateways, PoPs, routes, the
planner, strategies, serving satellites and drops (`phase-1-orbits.md`, `phase-2-topology.md`
and `phase-3-routing.md`, section 2 of each). New here:

- **Failure.** An element (a satellite, a laser link or a gateway) that stops working at an
  instant nobody predicted.
- **Element.** One thing that can fail. A failed satellite carries nothing: all its lasers are
  dead and no dish or gateway can use it.
- **Failure instant.** The time the element stops. Written t_f.
- **Transient failure.** A failure that ends: the element works again after a set time.
- **True network.** What actually works at an instant.
- **View.** What one actor believes works. An actor that has not heard of a failure still
  believes the element works.
- **Fact.** One piece of news: "this element is down" or "this element is up again".
- **Detection.** A working neighbour of a failed element noticing it. Only neighbours detect:
  the two ends of a laser link, the satellites linked to a failed satellite, the dish or
  gateway that was using it.
- **Detection time.** How long a neighbour takes to notice, from the failure instant.
- **Reaction mode.** Who may change a route after a failure: the planner only, the planner
  plus local repair, or every satellite after flooding (4.5).
- **Local repair.** The node just before the failed element on a route switching, on its own,
  to a backup prepared in advance.
- **Flooding.** A node that learns a fact passes it to all its neighbours, who pass it on,
  until every node has it.
- **Table.** In the flooding mode, a node's own choice of next hop for each destination,
  worked out from its own view.
- **Black hole.** Traffic being sent towards a failed element, or to a node that has nowhere
  to send it. It is lost.
- **Loop.** Traffic passing the same node twice. It never arrives.
- **Acquisition time.** How long two lasers take to find each other and lock on before a new
  link carries anything.
- **Repair link.** A laser link made during a run between two lasers that lost their partners.
- **Settled.** Every actor's view is complete again and the route is the one a planner with
  full knowledge would hold.

## 3. What is already known

From the Phase 3 runs (`docs/phase-3-data/`):

- A backup route that shares no satellite with the main one costs a median 0.04 to 2.35 ms in
  the best case. So once a route has been re-planned, one lost satellite is cheap. The cost of
  a failure is the time until then.
- A route that breaks stays down until the next plan: up to 14 s at a 15 s re-plan.
- 9 of the 11 realistic directions with service use one gateway at 99.6% or more of steps.

From the ground data (`data/ground/2026-11-19/`), for the gateways those routes use:

- In the US the next gateway is 4 to 16 km from the one in use. Outside the US this project
  assumed one gateway per PoP, so the next is 518 to 2,524 km away.

From the sources:

- No public source gives Starlink's detection time, whether its satellites repair routes
  themselves, or the availability of its gateways.
- Laser acquisition: 100 s or less is the SDA standard's requirement; one vendor states 30 s
  the first time two terminals meet and 2 s every time after; a model puts low orbit at 30 to
  60 s.
- The outage of 2025-07-24 lasted about 2.5 hours and was a failure of the core network's
  software, not of satellites.
- No published failure study of a satellite network gives both its full setup and its numbers
  (6.3).

## 4. Design

### 4.1 Modules

| Module | What it holds |
|---|---|
| `network/` (extended) | Shortest delays from several sources at once; the pieces a graph falls into |
| `routing/` (extended) | Tables towards a destination, and following them hop by hop when nodes hold different views. Depends on `network` only |
| `topology/` (extended) | Re-matching freed lasers; link states that take failures and repair links; physical checks on a plan that changes |
| `failure/` (new) | The failure schedule and its file; which elements are down at an instant; who detects what and when. Depends on `topology` and the modules before it |
| `apps/failures/` (new) | The `failures` program |
| `apps/inputs/` (extended) | A plan time separate from the run's start |

`routing` still knows nothing about orbits or failures: it is given graphs and views.

### 4.2 The failure schedule

A run is given a list of failures. Each has an element, a failure instant and either no end
(permanent) or a length in seconds (transient).

| Element | Written as | What goes down |
|---|---|---|
| Satellite | its catalogue number | Its lasers, and every ground link to it |
| Laser link | the catalogue numbers of its two ends | That link |
| Gateway | its name | Every ground link to it, and its fibre |

A whole plane and a region are lists of satellites; the schedule has no separate kind for
them.

- A failure instant must be a step of the run, `start + k × step`, or lie before the run's
  start.
- **A failure dated before the run's start is old news:** every actor knows it from the
  first step, and its repair links (4.8) are already made and acquired if the failure is at
  least one acquisition time old. This is how the runs that look only at the network after
  recovery are set up (5).
- A transient failure's element works again at t_f plus its length, which need not be a step.
- The file is plain text with one failure per line, as the link file of Phase 3 (4.9 there):
  comment lines start with `#`, the header is
  `kind,norad_cat_id_a,norad_cat_id_b,gateway,start_utc,length_s`, and unused fields are
  empty. The kinds are `satellite`, `laser_link` and `gateway`. A gateway's name may hold a
  comma ("New York, NY"), so that one field is written in double quotes, with a quote inside
  it doubled. Reading refuses the whole file, naming the line, for a malformed line, an element
  that is not in the run (an excluded satellite, a link that is not in the laser plan, an
  unknown gateway, a name that more than one gateway has), an instant that is neither a step nor before the start, a negative
  length, and the same element failing twice at overlapping times.
- With an empty schedule the `failures` program gives exactly the `routing` program's routes
  (6.2).

### 4.3 Time

The satellites move in 1 s steps, as in Phase 3. Events of a failure and its recovery happen
in continuous time.

- **The true network at a time t** is the geometry of the last step at or before t (positions,
  which links have line of sight, every delay) with the failures, returns and repair links in
  force at t.
- So an event between two steps is placed on the frozen network of the step before it. Within
  one second a laser link's delay changes by microseconds, which is far below what this phase
  measures.
- A recovery that lasts past the next step carries on with that step's geometry.
- **A flood is worked out entirely on the network of the step in which it starts.** A flood
  crosses the constellation in well under a second at the default settings.
- This replaces, for the events of this phase only, Phase 3's rule that events between two
  steps are out of scope. Geometry is still known only at steps.

### 4.4 Detection

- A failure is detected by each working neighbour at t_f plus the detection time
  (`--detection-s`, default 0.1 s; 4.9).
- Neighbours of a failed laser link: its two ends.
- Neighbours of a failed satellite: every satellite with a laser link to it that was up at
  t_f, every dish with a ground edge to it, every gateway that sees it.
- Neighbours of a failed gateway: every satellite it sees. The planner also learns of it by
  itself after the detection time, since a gateway is part of the ground network.
- A link that was already down at t_f for another reason is detected when it would have come
  back up.
- **A return is detected at once.** When a transient failure ends, a laser link must first
  acquire (4.8); the instant it carries traffic, both ends know. A returned satellite's ground
  links work from the instant it returns.
- **What a neighbour learns** depends on the link that went silent:
  - Over a laser, a satellite cannot tell a dead link from a dead satellite: both look the
    same. The fact it holds and passes on is "this laser link is down".
  - Over a ground link, the fact is about the other end: a dish or gateway whose satellite
    went silent holds "this satellite is down", and a satellite whose gateway went silent
    holds "this gateway is down". In this model nothing else silences a ground link that is
    in view.
- **A view takes a satellite as down** when it holds "this satellite is down", or when it
  holds "this laser link is down" for every planned laser link of that satellite. A satellite
  with no working laser can pass nothing on, so the view stops using it for ground links too.
- A fact about an element is replaced by a later fact about the same element: "up again"
  after "down", and the other way round.

### 4.5 The three reaction modes

In all three, routes are planned as in Phase 3: at every re-plan instant, on a planning graph.
The difference is the planning graph and who may act between plans. **The planning graph is
built from a view, not from the true network:** a failed element the planner has not heard of
is still in it.

**`planner` (the Phase 3 baseline).**

- Each detecting satellite sends a report to the ground. It arrives when it reaches any
  working, operational gateway: the detection instant, plus the lowest delay from that
  satellite to a gateway over the true network (lasers and one radio hop down), plus the
  per-hop time of 4.9 for every hop. The gateways used for reports are all the operational
  ones, in both endpoint models.
- The planner knows a fact from the earliest report's arrival plus its processing time
  (`--planner-processing-s`, default 0). A fact a gateway detects itself needs no report.
- The planner acts on what it knows at its next re-plan instant. A plan takes effect at once,
  as in Phase 3.
- Nobody else changes a route. A dish whose serving satellite failed has no uplink until the
  planner gives it another at a slot start; a planner that has not heard may assign a dead
  satellite.
- Comparison: `--replan-on-report on` makes the planner re-plan every direction the instant it
  learns a fact, without waiting for the interval.

**`local-repair`.** Everything of `planner`, and:

- At every plan, each node of a held route except the last prepares a backup: the shortest
  path in the planning graph from that node to the route's end that avoids the next node of
  the route and every node before itself. Avoiding the next node also avoids the link to it,
  so one backup serves both a dead link and a dead satellite.
- When a node of the route detects that the next element is dead, it switches to its backup:
  from the detection instant the route is the old route up to that node, then the backup.
- In the realistic model the sending dish's backup starts at its open dish (Phase 3 spec 4.2),
  so its repair is a new serving satellite: the first satellite of that backup.
- One repair per route between two plans. A backup that itself meets a dead element is a black
  hole until the planner acts.
- The repaired route is held until the next plan made by a planner that knows the fact.

**`flooding`.** There is no planner for failures. Every satellite, dish and gateway keeps its
own view and its own table.

- At every re-plan instant every node works out its tables from the predicted network, as the
  planner would, minus the elements its own view says are down. Without failures every node
  has the same view, so the routes are those of the `lowest-delay` strategy; the flooding mode
  therefore runs with `lowest-delay` only.
- A table is the shortest-path tree towards the destination: a node's next hop is the next
  node on the lowest-delay path from itself to the destination in its view, with the tie rule
  of `network::shortest_paths` applied from the destination's side, so that two nodes with the
  same view never disagree.
- A detecting node starts a flood of the fact. A node that has the fact sends it once on each
  of its working laser and ground links, except the one it first came in on. A node receives
  it after the link's delay plus the per-hop time.
- So the time a node learns a fact is the lowest total of (delay + per-hop time) over all
  paths from any detecting node, in the true network without the failed element: a shortest
  path from several sources at once.
- A node's tables use a fact from the instant it learns it plus the compute time
  (`--compute-s`, default 0.1 s).
- Between plans, traffic follows the tables hop by hop from the sending dish. With every node
  on the same view this is the planned route. While views differ it may reach a dead element
  or a node with no next hop (a black hole) or pass a node twice (a loop).
- In the realistic model the sending dish, on learning that its serving satellite is dead,
  picks again by its serving rule among the satellites its view says work.
- "Up again" facts and new repair links are flooded the same way.

### 4.6 What a direction experiences

At every instant each direction, in each reaction mode, is in one state:

| State | When |
|---|---|
| `delivered` | The traffic reaches its end over elements that all work. It has a route and a delay |
| `black_hole` | It is sent to a failed element, or reaches a node with no next hop |
| `loop` | It passes a node twice |
| `no_route` | No route is held, or the held route broke for a reason that is not a failure: the drop causes of Phase 3 spec 4.6, unchanged |

A drop caused by a failure names the element: `failed_satellite`, `failed_link` or
`failed_gateway`, first one met along the route. A plan that finds no route in its view is
`no_path` or `no_gateway` as in Phase 3.

The state only changes at a step, a plan, a detection, a report's arrival, a node learning or
starting to use a fact, a return, or the end of an acquisition. Between two such instants it
is constant, so the program writes it as intervals in continuous time (4.10).

**The baseline.** In the same pass the program also runs each direction with no failures at
all, same settings. Every measure of extra delay or lost time is against that baseline at the
same instant, so drops that have nothing to do with the failure cancel out.

### 4.7 Measures

Per failure, direction and reaction mode:

- **Black-hole time and loop time:** seconds in each state from t_f to the end of the run.
- **Time to restore:** from t_f to the first instant the direction is `delivered` again after
  being interrupted. Zero if it never was interrupted; none if it never came back in the run.
- **Time to settle:** from t_f to the instant after which the route is the one a planner with
  full knowledge holds, and stays so.
- **Extra delay:** delivered delay minus the baseline's at the same instant, while both are
  delivered; and the same for the route once settled.
- **Lost time:** seconds the baseline is delivered and this mode is not.
- **Messages:** in `planner` and `local-repair`, the reports sent and their hops. In
  `flooding`, every sending of every fact on a link.

Per run, over a set of city pairs at one instant, for the runs that look at the network after
recovery (5):

- **Reachability:** the share of pairs with a route in the true network, every actor knowing
  everything.
- **Extra delay** of those routes against the same instant with nothing failed.
- **Largest piece:** the share of working satellites in the largest set joined by working
  lasers, per shell and overall.

### 4.8 Re-pointing lasers

With `--repoint on` a laser whose link failed looks for a new partner. `off` is the
comparison: it stays idle.

- **When.** At the first step at or after the instant both the laser's own satellite has
  detected the loss and the laser is free.
- **Which lasers.** A laser is free when its link is failed, or when it never had a partner
  (`no_free_partner` in the plan). A laser of a failed satellite is not free.
- **In a plane.** The two neighbours of a dead satellite, one with a free laser facing ahead
  and one facing behind, link to each other across the gap.
- **Across planes.** The matching rule of Phase 2 spec 4.3, on the free lasers only: pairs of
  free lasers on facing sides in neighbouring planes of one shell, shortest first at that
  step, accepted if the link has line of sight now and is permanent. Ties to the lower
  catalogue numbers.
- **Acquisition.** A repair link carries nothing for the acquisition time
  (`--acquisition-s`, default 30 s) after it is matched. While it waits it is down with the
  reason `acquiring`.
- **Learning of it.** A repair link is a fact like any other. Its two ends know when it comes
  up; the planner or the other nodes learn as in 4.5.
- **A return.** When a failed element works again, the original plan comes first: every repair
  link that uses a laser whose original partner is back is dropped at that instant, and the
  original link acquires again for the acquisition time before it carries traffic. Without
  re-pointing a returned laser link also waits the acquisition time.
- A freed laser can only pair with a free laser facing the right way, so re-pointing repairs
  only part of what is lost.
- Matching needs both satellites to agree on the new pair. How they agree is not modelled: the
  rule is deterministic and takes no time beyond detection.

The acquisition values run: 2 s, 12 s, 30 s and 100 s. 30 s and 2 s are one vendor's first
and later acquisitions, 100 s is the SDA standard's limit, and 12 s is the fastest SpaceX is
reported to have stated, which this project could not check at its source.

### 4.9 Times that are settings

SpaceX publishes none of these. Each is a setting with a default and a sweep.

| Setting | Default | Sweep | Where the values come from |
|---|---|---|---|
| `--detection-s` | 0.1 s | 0.01, 0.1, 1 and 40 s | No public figure for a laser link. The range runs from fast link-failure detection on the ground (RFC 5880) to a routing protocol's sample dead interval (RFC 2328, 40 s) |
| `--hop-processing-s` | 0.001 s | none | This project's assumption: the time a node takes to pass a report or a flooded fact on |
| `--compute-s` | 0.1 s | 0.01 and 0.1 s | This project's assumption: the time a node takes to work out new tables |
| `--planner-processing-s` | 0 s | none | This project's assumption |
| `--acquisition-s` | 30 s | 2, 12, 30 and 100 s | 4.8 |

### 4.10 The `failures` program

Input: everything the `routing` program takes (Phase 3 spec 4.8), and:

| Option | Values |
|---|---|
| `--failures` | The failure file. Without it the schedule is empty |
| `--reactions` | A list: `planner`, `local-repair`, `flooding` |
| `--replan-on-report` | `on`, `off` (default) |
| `--repoint` | `on`, `off` (default) |
| `--detection-s`, `--hop-processing-s`, `--compute-s`, `--planner-processing-s`, `--acquisition-s` | 4.9 |
| `--plan-time` | The reference time of the shells, planes and laser plan. Default: the run's start. With it a short run uses the same network as a longer one that started earlier |
| `--events-out`, `--facts-out` | The two CSVs below; not written without them |

One run steps through time once and evaluates the baseline and every listed reaction mode.
`--strategies` applies to `planner` and `local-repair`; `flooding` is `lowest-delay`.

Two options of the `routing` program are refused: `--lock-on-s` other than 0, because
`--acquisition-s` is this program's lock-on, and `--serving with-route`, because that rule
gives every strategy a dish of its own and the reaction modes here share one dish per city.

Output:

- **Routes CSV,** one row per step, direction, strategy and reaction mode (and the baseline):
  the Phase 3 columns, with the state of 4.6 at the step's instant and the element named by a
  failure drop.
- **Events CSV,** one row per interval of 4.6: its start and end in seconds from the run's
  start, the direction, strategy, reaction mode, state, route, delay and failed element.
- **Facts CSV,** one row per fact: the element, down or up, the instant, who detected it and
  when, when the planner knew, how many nodes the flood reached and when the last one learned,
  and the messages.
- **Summary:** the Phase 3 summary, the measures of 4.7, the laser plan's changes, and the
  checks of 6.1.

Exit status as the `routing` program: 0, 1 if any check of 6.1 fails, 2 for bad arguments or
inputs.

## 5. Runs

All on the snapshot of 2026-11-18 and the ground data of 2026-11-19, with the laser plan of
2026-11-18 21:15:00 UTC (`--plan-time`), 1 s steps, pattern `three`, the `whole-interval`
planner re-planning every 15 s, strategy `lowest-delay`, serving rule `highest-elevation`.
The `whole-interval` planner is used because Phase 3 showed it has no drops of its own in the
best case, so what is measured here is the failure.

**Failure instants.** Ten, T_i = start of the Phase 3 window + 300 s + i × 697 s, i = 0 to 9.
They fall 0, 7, 14, 6, 13, 5, 12, 4, 11 and 3 s after a re-plan instant, so the wait for the
next plan is spread over the interval. Each run starts at the last re-plan instant of that
window at least 60 s before T_i and ends 300 s after T_i.

**Hand-picked failures.** Chosen from the baseline's route for the direction at T_i, by
`python/make_failures.py`:

| Scenario | What fails |
|---|---|
| `route-satellite` | The middle satellite of the route |
| `access-satellite` | The first satellite of the route |
| `route-laser` | The middle laser link of the route; left out when the route has no laser hop |
| `plane` | Every satellite in the middle satellite's plane |
| `region` | Every satellite whose point on the ground is within 1,000 km of the middle satellite's at T_i |
| `gateway` | The gateway the route uses (realistic only) |

| Group | Varies | Fixed |
|---|---|---|
| Main | Every scenario, the three reaction modes, both models, the six study pairs one way, the ten instants; permanent | The defaults of 4.9, re-pointing off |
| Transient | `route-satellite` and `route-laser` lasting 10 s and 60 s | As main |
| Detection | Detection 0.01, 1 and 40 s; `route-satellite` and `route-laser` | As main |
| Compute | Compute 0.01 s; `flooding` | As main |
| Planner | `--replan-on-report on`; and the `start` planner | As main |
| Re-pointing | Re-pointing on with acquisition 2, 12, 30 and 100 s; `route-satellite`, `plane`, `region` | As main |
| Gateway | `gateway`, with the next-nearest gateway's distance; real and assumed gateways reported apart | As main, realistic |

**After recovery.** One step, best case, Hypatia's 100 cities (4,950 pairs, one way), every
failure dated an hour before the step so that everyone knows it (4.2):

| Group | What fails | Repeats |
|---|---|---|
| Random satellites | 0.1%, 0.3%, 1%, 3%, 10% and 30% of the satellites in planes | 100 seeds each, at T_0 |
| Random lasers | The same shares of the planned laser links | 100 seeds each, at T_0 |
| Attack | The same shares of satellites, taking first those that carry the most of the 4,950 baseline routes at that instant | Once at each of the ten instants |
| Re-pointing | Random satellites and the attack again, re-pointing on | As above |
| Pattern | Random satellites with `plus-grid` and `six` | 100 seeds each |

The random choices are made by `python/make_failures.py` from a seed with numpy's `default_rng`
and written to the failure files; the seeds are 0 to 99 and are recorded in the summaries.

The real rate of unannounced losses is reported only as context: about one satellite a week
across the fleet would mean about 0.01 failures in a two-hour run.

## 6. How it is shown to be right

### 6.1 Checks in the program

Counted in the summary; any failure gives exit status 1. Phase 3's checks (6.1 there) still
run on the baseline and on every `delivered` interval. New:

- Every edge of a `delivered` route is up in the true network: no failed element, no repair
  link still acquiring.
- A `black_hole` interval's traffic does reach a failed element or a node with no next hop,
  and a `loop` interval's does pass a node twice.
- No actor uses a fact before it can have it: no node before t_f plus the detection time, the
  planner not before the earliest report can arrive, no table before the node's learning time
  plus the compute time.
- When a flood is done, every node that the true network joins to a detecting node has the
  fact.
- Once a failure is settled, the route in every reaction mode is the route a planner with
  full knowledge holds, and in `flooding` following the tables from every node of that route
  gives the rest of it.
- With re-pointing off, no delivered delay is below the baseline's ideal at that step: taking
  elements away never shortens the shortest path.
- At every step each laser is in at most one link that is not failed, every repair link joins
  two satellites of one shell and has line of sight, and no link to a failed satellite is up.
  The physical checks of Phase 2 otherwise run unchanged on the links in force.

### 6.2 Agrees with Phase 3

With no failure file, the baseline and every reaction mode must give the `routing` program's
rows on the same settings: status, drop cause, route, plan, delays to 1e-9 ms. `flooding` is
held to the `lowest-delay` rows. Both models, real snapshot, as a check executable built
without sanitizers.

### 6.3 Agrees with an independent model

A Python model written from this spec, sharing no code with the program
(`python/validate_failures.py`), must give the same intervals, facts and measures as the
program, on Hypatia's ideal shell with its link file and on a `three` plan of the same shell,
best case. It is the proof of this phase, as in Phases 2 and 3. A short version runs in CI as
`validate-failures`, with no network access.

What it must reproduce exactly (times to 1e-9 s, delays to 1e-9 ms):

- **Flood times.** Each node's learning time, from the model's own shortest path from several
  sources with (delay + per-hop time) on every link.
- **Report times.** The planner's knowing time for every fact.
- **Every interval** of every direction in the three reaction modes: state, route, delay and
  element, for permanent and transient failures of a satellite and of a link.
- **Messages** of every flood.
- **Repair links:** which lasers pair, and when each link carries traffic.
- **Reachability and the largest piece,** from the model's own count of connected pieces
  (union-find, not a shortest path).

The realistic model and gateway loss are not in the independent model, as in Phase 3. They
rest on the program's checks, the tests of 6.5 and 6.2.

**No published result is reproduced.** A published number only proves something under a
matching setup, and no failure study that could be opened gives both. The closest, StarGlider,
fails one link on the shortest path between random satellite pairs and reports that the best
backup from the node next to the failure costs over 20% more delay in 7% of 1,000 runs; its
text gives two different sizes for its shell. So nothing outside this project confirms the
failure numbers. That is weaker than Phases 2 and 3, and the report says so.

### 6.4 Compared, with no pass or fail

- **StarGlider's 7%.** On Hypatia's shell, `local-repair`, one link failed on the route of
  1,000 randomly drawn pairs of Hypatia's cities: the share whose repaired route is over 20%
  slower than the route before the failure. Reported next to 7% with every difference of
  setup named.
- **Percolation.** On an endless square grid the network falls apart when half the links, or
  about 41% of the nodes, are gone; on a honeycomb grid, which is what `three` makes of an
  ideal shell, at about 35% of the links or 30% of the nodes. On Hypatia's shell of 72 by 22
  the largest piece against the failed share should bend near those values, blurred by the
  shell's size. A shape, not a tolerance.
- **Real outages.** The outage of 2025-07-24 is described as what this model cannot show: a
  failure of the core network's software. Measured dish-level outages are mostly handovers of
  under 2 s, which belong to Phase 3.

### 6.5 Tests

- `network`: shortest delays from several sources against the lowest of the single-source
  results; pieces of hand-made graphs.
- `routing`: tables on small graphs; following tables with mixed views to a hand-worked black
  hole, loop and delivery; the same view at every node gives the planner's route.
- `topology`: re-matching on a synthetic shell where the freed lasers and their only possible
  partners are known; a return restoring the original link; the acquisition wait; each new
  physical check made to fire.
- `failure`: the file's round trip and every refusal; elements down at instants around a
  transient failure's start and end; who detects each kind of element.
- `failures_app`: each reaction mode on a synthetic shell with every interval worked out by
  hand, for a satellite, a link and a gateway; the planner's stale plan choosing a dead
  element; local repair onto a backup that is itself dead; old failures known from the first
  step; every check of 6.1 made to fire.

Sanitizer tests stay on small shells. Anything that reads the full snapshot is a check
executable built without sanitizers.

## 7. Out of scope

- Traffic load, queues, packets and TCP. A black hole is counted in seconds, not in packets.
- Failures that are partial: a slower or lossier link.
- PoPs, fibre and the core network's software failing.
- Neighbours exchanging distance tables.
- Re-pointing lasers for gaps that can be predicted, and re-matching a whole shell.
- How two satellites agree on a repair link, and how a plan reaches the satellites.
- Spare satellites and replacement, which take weeks.
- Solar storms as a change of orbits.
- Geometry between two 1 s steps.

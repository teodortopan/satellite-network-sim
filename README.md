# satellite-network-sim

[![CI](https://github.com/teodortopan/satellite-network-sim/actions/workflows/ci.yml/badge.svg)](https://github.com/teodortopan/satellite-network-sim/actions/workflows/ci.yml)

A simulator of a Starlink-like satellite network on real public orbital data.

**Status:** Phase 1 (orbits) is implemented. From a saved snapshot of CelesTrak element sets and
IERS Earth-orientation data, the `propagate` program computes every Starlink satellite's
position with SGP4, in the Earth-fixed frame (ITRF), as latitude, longitude and height
(WGS-84), and in J2000. It agrees with Skyfield to under 1 mm in TEME and ITRF and is within
0.5 km (median) of SpaceX's own trajectories near the element epoch. The numbers, the plots
and how to reproduce them are in [docs/phase-1-results.md](docs/phase-1-results.md).

Phase 2 (network topology) is implemented. The `topology` program turns those positions into
a network that changes over time: 8,395 satellites in 300 orbital planes of four shells,
linked by lasers in three patterns, with gateways, points of presence and fibre on the
ground. For each pair of cities it reports the delay every 15 seconds under two models:
Starlink as an internet provider (traffic comes down near the sender and continues by
fibre) and Starlink as a backbone (space only). Over 12 hours, New York to London takes a
median 40 ms one way as a provider and 21 ms as a backbone, against a physical lower bound
of 19 ms. Every network passes physical checks at every step, and the program agrees with an
independent model to 1e-6 ms on an ideal shell. The numbers, the plots and the assumptions
are in [docs/phase-2-results.md](docs/phase-2-results.md).

Phase 3 (routing) is implemented. The `routing` program follows each route through time: a
planner chooses a route, holds it and re-plans on a fixed interval, under four strategies
(lowest delay, keep until broken, longest lived, switch only above a threshold). It reports
how often a route changes, how much the delay jumps, how long there is no route and why.
Over two hours at 1 s steps, a backbone route re-planned every 15 s on one instant's
positions is down 6% to 19% of the time on five of six city pairs, because the lowest-delay
route uses satellites about to leave view; planning with the links predicted to last the
whole interval removes every one of those drops for at most 0.2 ms of median delay. As an internet provider,
the dish's serving satellite changes at 41% to 76% of the 15 s slots and every strategy but
one gives the same route. The program agrees row for row with an independent model,
reproduces Hypatia's published median of path changes and comes within 8.1% of a second
paper's counts.
The numbers, the plots and the limits are in
[docs/phase-3-results.md](docs/phase-3-results.md).

Phase 4 (failures) is implemented. The `failures` program stops a satellite, a laser link or
a gateway without warning and follows what each route goes through, under three ways of
reacting: a planner on the ground that waits for reports, a node next to the failure that
switches to a backup path at once, and satellites that tell each other and work out their own
routes. One dead satellite costs the planner a median of 8.5 s without a route (the wait for
its next plan), local repair 0.1 s and flooding 0.2 s, and flooding pays for it with about
144,000 message sendings against 6 report hops. With 10% of the satellites failed at random
every one of 4,950 city pairs still has a route; at 30%, 94% do, and 99.999% once freed
lasers are re-pointed. The program agrees with an independent model on every interval, fact
and flood on an ideal shell. The numbers, the plots and the limits are in
[docs/phase-4-results.md](docs/phase-4-results.md).

Phase 5 (the dashboard) is implemented and published at
<https://teodortopan.github.io/satellite-network-sim/>. `dashboard/` is a web page that replays saved engine
runs on a globe: one failure under the three ways of reacting side by side, a route through
two hours under four strategies, the constellation with its shells and laser patterns, and
the network falling apart as satellites fail. The browser simulates nothing. It only moves
satellites between positions saved every 60 s (off by 12 m at most) and formats numbers, and
a data check compares every number the page loads with the engine's own output. What was
built, how it is checked and its limits are in
[docs/phase-5-results.md](docs/phase-5-results.md).

This repository is the source of truth for the project.

## Build and test

The only host requirement is Docker. The check builds the CI image and runs every step in it
(formatting, Python lint, ASan/UBSan builds with GCC and Clang, Valgrind, clang-tidy, the
comparison of `propagate` against Skyfield, and the comparisons of `topology`, `routing` and
`failures` against independent models):

```sh
scripts/check.sh            # all steps
scripts/check.sh asan-gcc   # a single step
```

See [docs/ci.md](docs/ci.md) for what each step catches.

## Propagating a snapshot

`propagate` (built to `build/<preset>/bin/propagate`) turns a saved snapshot and a time range
into one CSV row per satellite per time: TEME, ECEF (ITRF), WGS-84 geodetic and J2000 (FK5)
positions, with each satellite's element source and element age. A summary goes to standard
output (standard error when the CSV goes to standard output).

```sh
propagate --snapshot data/starlink/2026-11-18 --start 2026-11-18T21:00:00 \
  --duration-s 3600 --step-s 60 --out positions.csv [--ids 44714,44718]
```

Every time must lie inside the snapshot's Earth-orientation file (`finals2000A.all`);
otherwise the run fails before writing anything. Satellites that fail to propagate are
counted in the summary and get no rows. `propagate --help` lists the options.

## Delay between cities

`topology` (built to `build/<preset>/bin/topology`) writes one CSV row per city pair,
direction and time step: the delay, the hops, the path's parts, the lower bound, and which
satellite, gateway and point of presence were used.

```sh
topology --snapshot data/starlink/2026-11-18 --ground data/ground/2026-11-19 \
  --cities data/ground/cities.json --start 2026-11-18T21:15:00 \
  --duration-s 43200 --step-s 15 --pattern three --model realistic --out delays.csv
```

`--pattern` is `three`, `plus-grid` or `six` lasers per satellite; `--model` is `realistic`
or `best-case`. `topology --help` lists the limits that can be changed.

## Routes over time

`routing` (built to `build/<preset>/bin/routing`) writes one CSV row per time step, city
pair, direction and strategy: whether the held route still works, its delay against the
lowest delay possible at that step, the route itself, and the cause of every drop.

```sh
routing --snapshot data/starlink/2026-11-18 --ground data/ground/2026-11-19 \
  --cities data/ground/cities.json --start 2026-11-18T21:15:00 \
  --duration-s 7200 --step-s 1 --pattern three --model best-case \
  --strategies lowest-delay,keep-until-broken,longest-lived,threshold:1ms \
  --replan-s 15 --out routes.csv --drops-out drops.csv
```

`--planner` is `start` or `whole-interval`; `--serving` chooses how a dish picks its
satellite in the realistic model. `routing --help` lists every option.

## License

MIT. See [LICENSE](LICENSE).

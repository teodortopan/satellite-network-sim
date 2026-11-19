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
are in [docs/phase-2-results.md](docs/phase-2-results.md). Routing, failures and the
dashboard come in later phases.

This repository is the source of truth for the project.

## Build and test

The only host requirement is Docker. The check builds the CI image and runs every step in it
(formatting, Python lint, ASan/UBSan builds with GCC and Clang, Valgrind, clang-tidy, the
comparison of `propagate` against Skyfield, and the comparison of `topology` against an
independent model):

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

## License

MIT. See [LICENSE](LICENSE).

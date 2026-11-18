# satellite-network-sim

[![CI](https://github.com/teodortopan/satellite-network-sim/actions/workflows/ci.yml/badge.svg)](https://github.com/teodortopan/satellite-network-sim/actions/workflows/ci.yml)

A simulator of a Starlink-like satellite network on real public orbital data.

**Status:** Phase 1 (orbits) is implemented. From a saved snapshot of CelesTrak element sets and
IERS Earth-orientation data, the `propagate` program computes every Starlink satellite's
position with SGP4, in the Earth-fixed frame (ITRF), as latitude, longitude and height
(WGS-84), and in J2000. It agrees with Skyfield to under 1 mm in TEME and ITRF and is within
0.5 km (median) of SpaceX's own trajectories near the element epoch. The numbers, the plots
and how to reproduce them are in [docs/phase-1-results.md](docs/phase-1-results.md). Links,
routing and the dashboard come in later phases.

This repository is the source of truth for the project.

## Build and test

The only host requirement is Docker. The check builds the CI image and runs every step in it
(formatting, Python lint, ASan/UBSan builds with GCC and Clang, Valgrind, clang-tidy, and the
comparison of `propagate` against Skyfield):

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

## License

MIT. See [LICENSE](LICENSE).

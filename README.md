# satellite-network-sim

[![CI](https://github.com/teodortopan/satellite-network-sim/actions/workflows/ci.yml/badge.svg)](https://github.com/teodortopan/satellite-network-sim/actions/workflows/ci.yml)

A satellite network simulator.

**Status:** early. C++20 build, tests and CI are in place; simulator code has not started yet.

This repository is the source of truth for the project.

## Build and test

The only host requirement is Docker. The check builds the CI image and runs every step in it
(formatting, ASan/UBSan builds with GCC and Clang, Valgrind, clang-tidy):

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

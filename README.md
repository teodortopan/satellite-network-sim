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

## License

MIT. See [LICENSE](LICENSE).

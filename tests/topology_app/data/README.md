# Test data for tests/topology_app

Invented ground data for the topology program's tests; nothing here is real.

- `cities.json`: four cities and three pairs. Alpha (US-NY), Beta (US-IL) and Delta (JP-13)
  each have one geofeed row mapping to their own PoP (`pa`, `pb`, `pd`); Gamma (FR) has
  none, so it has no home PoP (no service).
- `ground/`: a complete ground-data directory: two operational gateways near Alpha and
  Beta, one planned gateway near Delta (never used), three PoPs, their address blocks and
  the geofeed. No `assumed_gateways.json`.
- `assumed_gateways.json`: one operational gateway near Delta, copied next to `ground/`'s
  files by the tests of assumed gateways.

The snapshot these run on is written by the tests themselves (`fixture.cpp`): an ideal
Walker shell of 160 satellites that the default classification puts in its 53deg shell.

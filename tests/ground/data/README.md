# Test data for tests/ground

Hand-made fixtures for the loaders' error paths. Each `*_mixed` file holds good records and
one bad record per problem the loader must report; `loaders_test.cpp` lists what each record
is for. The good rows of `starlink_pops_mixed.csv`, `starlink_geofeed_mixed.csv`,
`gateways_mixed.json` and `pop_codes_mixed.json` are copied from the committed data in
`data/ground/2026-11-19/`; everything else is invented.

`starlink_pops_mixed.csv` and `starlink_geofeed_mixed.csv` contain one CRLF line each, and
the geofeed fixture has no newline at the end; `.gitattributes` keeps them unconverted.

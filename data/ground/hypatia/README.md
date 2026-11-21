# Hypatia's 100 cities

The ground stations of the Hypatia paper's path-change results (Kassing et al., "Exploring
the 'Internet from space' with Hypatia", IMC 2020, Figure 8a), used by the reproduction of
`docs/phase-3-routing.md` 6.3 and the `validate-routing` CI step.

| File | What it is |
|---|---|
| `ground_stations_cities_sorted_by_estimated_2025_pop_top_100.basic.txt` | Hypatia's file, unchanged: `id,name,latitude,longitude,elevation_m`, the 100 largest cities by estimated 2025 population |
| `cities.json` | The same cities in this project's format, with all 4,950 pairs from the lower id to the higher, written by `python/make_hypatia_cities.py` (its docstring gives the rules) |

Source: [github.com/snkas/hypatia](https://github.com/snkas/hypatia),
`paper/satellite_networks_state/input_data/ground_stations_cities_sorted_by_estimated_2025_pop_top_100.basic.txt`
(sha256 `7432ad4abf5d2159ed9351080599657604e210095b73874e3e51d7fb9871ab62`), copied
2026-11-20. Hypatia's `paper/` directory is under the MIT licence:

```text
The MIT License (MIT)

Copyright (c) 2020 ETH Zurich

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Hypatia places these stations on the WGS-72 ellipsoid; this project uses WGS-84, at height
0 as Hypatia does. The two place a city about 2 m apart, so a ground link at the
1,089.686 km limit can start or end a fraction of a millisecond earlier or later.

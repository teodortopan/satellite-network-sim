# Ground data

Where Starlink traffic meets the ground: gateways, points of presence (PoPs) and the
cities this project studies. Written by `python/fetch_ground.py`, except where noted.

| File | What it is | Source |
|---|---|---|
| `cities.json` | The cities and city pairs of the study, plus the validation-only pairs. | Hand-made. |
| `<date>/starlink_pops.csv` | Address block, router name, PoP code. As received. | SpaceX, `geoip.starlinkisp.net/pops.csv` |
| `<date>/starlink_geofeed.csv` | Address block, country, region, city. As received. | SpaceX, `geoip.starlinkisp.net/feed.csv` |
| `<date>/gateways.json` | Gateway sites: name, latitude, longitude, status. | Community dataset (below). |
| `<date>/pop_locations.json` | PoP sites: city, country, latitude, longitude. | Community dataset (below). |
| `<date>/pop_codes.json` | One row per PoP code in `starlink_pops.csv`: its routers, city and coordinates. | Hand-made (below). |
| `<date>/metadata.json` | Source URL, fetch time, sha256 and size of every download. | `fetch_ground.py` |

## The community dataset

SpaceX publishes no list of gateway sites. `gateways.json` and `pop_locations.json` are the
rows of [juliensimon/starlink-ground-stations](https://huggingface.co/datasets/juliensimon/starlink-ground-stations)
by Julien Simon, licensed [CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/), copied
unchanged at the revision recorded in `metadata.json`. It is built from FCC earth-station
filings, Starlink Insider and PeeringDB. It is unofficial: sites may be missing, planned or
retired, and its coordinates were partly found by looking place names up automatically.

## `pop_codes.json`

SpaceX's file names each PoP by a short code (`lhr`, `ord`) and a router name (`lndngbr1`),
with no location. This table gives each code a city and coordinates. The city was matched
by hand from the code and router name. Coordinates are the community dataset's where it has
that city, and the city centre otherwise. Two community rows are in the wrong place and are
replaced by the city centre: Ashburn (placed in Georgia, not Virginia) and Brasília (placed
at the centre of Brazil). The code `staging2` is left out: it is not a public PoP.

Coordinates are good to city level (a few tens of kilometres at worst), which is about
0.1 ms of fibre delay.

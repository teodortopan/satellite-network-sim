# The dashboard

A web page that shows the simulator's Starlink-like network on real orbital data (Phase 5, spec
[docs/phase-5-dashboard.md](../docs/phase-5-dashboard.md)).

- **The landing page, "Try it"** (`#explore`): a visitor picks any two places (by name, or by
  clicking the globe) and the routing program, compiled to WebAssembly, works out the route
  between them in the visitor's browser, in a Web Worker. This is the one thing computed in the
  browser. It is vouched for by the engine check (`scripts/check-engine.ts`, part of
  `scripts/ci.sh dashboard`), which runs the same exported WebAssembly program the same way
  (`src/engine/run.ts`, `src/engine/from-csv.ts`) and holds it to the native routing program
  and to the saved route story; the browser test (`e2e/explore.spec.ts`) checks that the page
  shows exactly the routes CSV the worker returned. Until the visitor's own run is ready, the
  saved route story's first pair plays. The address keeps the choice
  (`#explore?from=Lagos,6.4452,3.3896&to=...`), so a run can be shared.
- **The saved runs**: the page loads files the engine wrote, moves satellites between saved
  positions (spec 4.3) and formats numbers; nothing of them is simulated in the browser.

Vite, strict TypeScript, three.js for the globe, uPlot for the charts, no UI framework.

## The data

The page reads `public/data/`, which is built, not committed:

```sh
# From the repository root, inside the CI image (the engine must be built):
python3 python/export_dashboard.py --small   # seven minutes, for tests (about 30 s)
python3 python/export_dashboard.py           # two hours, for the real site (about 5 min)
python3 python/check_dashboard_data.py       # compares every exported number with the engine
```

The files and their fields are described in [data-format.md](data-format.md). The export also
keeps the engine's CSV files in `results/dashboard/<small|full>/`; the browser test reads them.

The page loads `manifest.json` first, refuses any file whose size is not the manifest's, and
checks the shape of every JSON file (fields, types, index ranges) before drawing anything from
it. A refused file is named on the page with the reason.

## Commands

Run in this directory, with Node 22:

```sh
npm ci              # install exactly the versions in package-lock.json
npm run dev         # local server with reloading, at http://localhost:5173/
npm run typecheck   # tsc, strict, for the page and for the test and config files
npm run lint        # ESLint (typescript-eslint, strict, type-checked), no warnings allowed
npm test            # unit tests (Vitest)
npm run build       # the static site in dist/, data included, relative paths only
npm run preview     # serve dist/
npm run e2e         # browser test (Playwright, headless Chromium) against dist/
```

`npm run e2e` needs a build made with the data in `public/data/` and the engine's CSV files of
the same export in `results/dashboard/<mode>/` (or set `SATSIM_RESULTS` to their directory). It
stops at once with a message if `dist/data/manifest.json` is missing. With
`SATSIM_SCREENSHOTS=1` it also writes the report's screenshots to `docs/img/phase-5/` (one wide
shot per story, and the failure story on a phone). There is one spec file per story in `e2e/`;
what they share (reading the engine's files, opening a story, moving the clock, counting a
globe's grey levels, checking that no pixel has a hue, comparing a shown number with the engine's) is in `e2e/helpers.ts`. Chromium
comes from `PLAYWRIGHT_BROWSERS_PATH` (`/opt/pw-browsers` in the CI image); the version of
`@playwright/test` is pinned to match it.

`.npmrc` sets `legacy-peer-deps`: npm 10 fails with "Cannot read properties of null (reading
'edgesOut')" while resolving Vitest's optional browser peers. Every peer this project needs is
listed in `package.json`.

## Layout of `src/`

| Path | What |
|---|---|
| `main.ts` | Loads the manifest and the shared files, builds the story menu, mounts a story |
| `data/` | The checks (`check.ts`), the loader with the size check (`loader.ts`), `manifest.ts`, the shared files (`common.ts`) |
| `clock/` | The clock's logic (`clock.ts`, no DOM) and its controls (`clock-view.ts`) |
| `globe/` | Interpolation (`interpolation.ts`), ECEF and latitude/longitude to the scene and back, for a click (`coords.ts`), the renderer and views (`globe-view.ts`), what is drawn (`layers.ts`), the shared satellite positions and links (`satellite-field.ts`), sites and routes in the scene (`paths.ts`), one globe in one element (`stage.ts`) |
| `charts/` | What every uPlot chart shares: the axis look, a chart that follows its element's width, line charts with a legend that reads the values under the cursor |
| `ui/` | Shared controls: one-of-several buttons, switches, drop-down lists, the frame loop |
| `stories/` | `story.ts` (the interface), `index.ts` (the registered stories), one folder per story |
| `stories/explore/` | "Try it", the landing page: two place boxes, the run in the browser, the headline (`state.ts`: the address, picked points, the headline from the engine's output) |
| `engine/` | The routing program in WebAssembly: one run (`run.ts`), its output to the route story's types (`from-csv.ts`), the manifest's `engine` block (`settings.ts`), the Web Worker that runs it (`worker.ts`, `protocol.ts`) and the page's side, which downloads and compiles once and runs one request at a time (`client.ts`) |
| `data/places.ts` | `places.json`, the places a visitor can search |
| `ui/place-search.ts` | The place box: a combobox with suggestions (name prefix, then word start, then anywhere; accents and case ignored; larger places first) |
| `stories/failure/` | One failure, three reactions (spec 4.5) |
| `stories/route/` | A route through time (spec 4.6) |
| `stories/constellation/` | The constellation (spec 4.7) |
| `stories/resilience/` | The network falling apart (spec 4.8) |
| `format.ts`, `theme.ts`, `styles.css` | Number formatting, the greys and the style of every series, the page's look |
| `ui/marks.ts` | The dot shapes and line styles drawn on a chart's canvas and as the swatch beside a name |

Each story folder has `data.ts` (its file, read and checked), `state.ts` (what is shown at a time
or for a menu choice: lookups only), their unit tests on a hand-made `fixture.ts`, and
`index.ts` (the view). The page is black, white and greys only (`e2e/greyscale.spec.ts` checks
that no pixel has a hue). Each routing strategy, laser pattern, kind of failure, reaction mode
and shell has one style in `theme.ts` (a grey, a line width and dash pattern, a dot shape), used
alike on the globe, in the charts and in the swatch beside its name; a dropped route is dotted
and dimmer, a failed element is a cross, a satellite with the news a ring. The clock bar sits in
the page flow right below the globes, never over them.

A story implements `Story` in `stories/story.ts` and is added to `STORIES` in
`stories/index.ts`; only registered stories appear in the menu.

The three lanes of the failure story are three views of one WebGL renderer: one canvas behind
the lanes, each lane's globe drawn into its own viewport with the scissor test. One WebGL
context, one upload of the 8,395 satellite positions per frame, and one camera and set of
controls, so the lanes always show the same region.

`globe/interpolation-reference.json` is written by `python/make_interpolation_reference.py`
with the data check's own interpolation; the unit test holds the page's interpolation to it.

## Licences of bundled data

- Land outlines: [Natural Earth](https://www.naturalearthdata.com/about/terms-of-use/) 1:110m
  land, public domain, through the npm package `world-atlas` 2.0.2 (ISC licence,
  Mike Bostock), bundled into the page.
- Place names: [Natural Earth](https://www.naturalearthdata.com/about/terms-of-use/) 1:10m
  populated places (v5.1.2), public domain, exported as `places.json`.
- three.js (MIT), uPlot (MIT) and topojson-client (ISC) are bundled into the page.

The page loads nothing from other sites: no fonts, scripts, trackers or cookies. The engine's
program is loaded from the page's own data directory and imported from a `blob:` URL.

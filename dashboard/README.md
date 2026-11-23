# The dashboard

A web page that replays the engine's saved runs (Phase 5, spec
[docs/phase-5-dashboard.md](../docs/phase-5-dashboard.md)). Nothing is simulated in the browser:
the page loads files the engine wrote, moves satellites between saved positions (spec 4.3) and
formats numbers.

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
`SATSIM_SCREENSHOTS=1` it also writes the report's screenshots to `docs/img/phase-5/`. Chromium
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
| `globe/` | Interpolation (`interpolation.ts`), ECEF and latitude/longitude to the scene (`coords.ts`), the renderer and views (`globe-view.ts`), what is drawn (`layers.ts`), the shared satellite positions (`satellite-field.ts`) |
| `stories/` | `story.ts` (the interface), `index.ts` (the registered stories), one folder per story |
| `stories/failure/` | One failure, three reactions (spec 4.5) |
| `format.ts`, `theme.ts`, `styles.css` | Number formatting, colours, the page's look |

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
- three.js (MIT), uPlot (MIT) and topojson-client (ISC) are bundled into the page.

The page loads nothing from other sites: no fonts, scripts, trackers or cookies.

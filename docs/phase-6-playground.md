# Phase 6: Any two places, in plain words, in black and white

**Status:** decided and specified 2026-11-27, implemented on the same day. It changes the
dashboard of Phase 5 ([phase-5-dashboard.md](phase-5-dashboard.md)); where the two specs
differ, this one holds.

## 1. Problem

The dashboard of Phase 5 replays six city pairs that the engine saved ahead of time. A
visitor could not ask about the two places they care about. The page also opened on its
hardest story with two long paragraphs of terms, and it told its lines and dots apart by
colour alone.

Phase 6 changes three things:

1. A visitor picks any two places on Earth and sees the route between them, computed when
   they ask.
2. A person who has never seen the project, and who is not technical, understands within a
   few seconds what the page shows and what to press.
3. The page is black, white and greys on a black background.

The saved runs of Phase 5 stay, as reference.

## 2. Background: the terms this spec uses

- **WebAssembly (wasm).** A program format that browsers run directly, at close to the
  speed of a normal program. A C++ program can be compiled to it.
- **Emscripten.** The compiler that turns C++ into WebAssembly, with a small JavaScript file
  that loads it.
- **On-demand run.** One run of the routing program started by the visitor's choice of two
  places, in the visitor's browser.
- **Web Worker.** A second thread of a web page. Work done there does not freeze the page.
- **Best-case model.** Phase 3's "Starlink as a backbone": a dish at each place may use any
  satellite above its elevation mask, and the whole trip stays in space. It needs only the
  two coordinates.
- **Native engine.** The routing program built for the computer it runs on, as in Phases 3
  to 5: the one every earlier check is about.

## 3. Any two places

### 3.1 The engine runs in the browser

The routing program is compiled to WebAssembly from the same source, with no change to any
engine library (`apps/routing_wasm` is a thin entry over the routing library). The page
runs it in a Web Worker.

This reverses decision 2 of Phase 5 ("playback of saved runs, no engine in the browser").
The page is still a static site with no server. The reason given then against WebAssembly
was that it "would be a second build of the engine to prove right"; section 3.4 is that
proof, and it is part of the check.

### 3.2 What an on-demand run is

- The best-case model, the laser pattern `three`, a re-plan every 15 s, planner `start`,
  backup on, and the plan time of Phases 3 to 5: the settings of the saved route story.
- Three strategies: lowest-delay, keep-until-broken and threshold 2 ms. Longest-lived is
  left to the saved runs: it looks 900 s ahead, which at 1 s steps holds 358 MiB of
  positions in the browser (measured) against 33 MiB without it.
- It starts at the start of the exported position window and lasts 600 s, or the whole
  window if that is shorter. It lies inside the window so that the globe draws the
  satellites from the exported positions.
- Both directions of the pair.
- The two places are written into a one-pair cities file with country and region `ZZ`, as
  Hypatia's cities are. The realistic model is not offered: it needs a home PoP, which
  comes from SpaceX's address feed by country, region and city name, and a free point has
  none.

### 3.3 Choosing the places

- By name, from Natural Earth's populated places (1:10m, public domain, 7,342 places;
  `python/fetch_places.py`, `data/ground/places/`). It is a map maker's selection, not a
  full gazetteer.
- By clicking the globe, for any point.

### 3.4 How the browser's engine is proved right

The `dashboard` step of the check builds the WebAssembly program and runs it in Node
(`dashboard/scripts/check-engine.ts`):

- **Against the saved story.** The saved pair, with the saved story's four strategies and
  window, run in WebAssembly and turned into the story's shape by the page's own converter
  (`dashboard/src/engine/from-csv.ts`), must reproduce `story-route.json`, which comes from
  the native engine through the Python export. Every interval, route, drop, cause and
  summary count must be equal, and every delay within 1e-9 ms.
- **Against the native program.** A run between two free points (64.1 N 21.9 W and 43.1 N
  131.9 E) with the three on-demand strategies must equal the native routing program's
  output for the same arguments: every non-numeric column, the drops file and the summary
  equal, every number within 1e-9 ms.

Measured on 2026-11-27: no difference in either comparison; the largest delay difference is
4.3e-14 ms. The delays differ in the last digits because WebAssembly's maths library is not
the one of the native build. A difference in a route, a drop or a count fails the check.

The data check (`python/check_dashboard_data.py`) holds the exported program to the built
one and every exported input to the repository's data, byte for byte.

### 3.5 What the page may compute

Phase 5's rule was that the page shows no number the engine did not compute, and that the
data check vouches for every number in the files. For an on-demand run the numbers come
from the engine in the browser, and the engine check of 3.4 vouches for that engine. The
page still works out nothing itself: it turns the engine's output into the route story's
shape (held equal to the export by 3.4), looks up the state at the clock's time, and
formats. The page says, beside an on-demand run, that it was computed in the browser.

### 3.6 Cost, measured in Node on 2026-11-27 (x86-64)

| | |
|---|---|
| Download of the program and its inputs | 15.4 MB, 2.8 MB compressed |
| Set-up per run | about 1 s |
| One simulated minute, three strategies | 0.85 s |
| Memory of the program | 33 MiB |
| The native program, same run of 420 s | 6.2 s against about 7 s |

## 4. A page a newcomer understands

- The landing page is the new story "Try it": two boxes for the places, one button, the
  globe. It opens on a saved run that is already playing, so the page is alive before any
  wait.
- Every story opens with a plain title, one or two sentences, and a "What to try" line.
  Longer explanations sit in two folds: "How to read this" and "Details and sources". File
  names, phase numbers and UTC times appear only in the second.
- Every globe has a legend beside it.
- Everyday names on screen for the terms of Phases 2 to 4: the strategies are rules
  ("Always the fastest", "Keep until it breaks", "Stay with long-lasting satellites",
  "Switch only if 2 ms faster"); the laser patterns are "3 / 4 / 6 lasers each"; a gateway
  is a ground station; a PoP is an Internet exchange; the planner is the control centre;
  the three reaction modes are "Wait for the control centre", "Detour on the spot" and
  "Spread the word".
- At 1440 by 900 the globe, its controls and Play are on the first screen.
- A failure is said in plain words: the same place twice, a place no satellite of the
  snapshot covers, a download that failed.

The story ids of Phase 5 (`#failure`, `#route`, `#constellation`, `#resilience`) are kept.

## 5. Black and white

- Every colour on the page has equal red, green and blue. The background is `#000000`.
- What colour told apart is told apart by grey level, line width, dash pattern and dot
  shape, and a swatch of the same mark stands beside every name.
- A browser test fails on any pixel with a hue, on every story.

## 6. Out of scope

- A server, accounts, or data that changes after the deploy.
- The realistic model, other laser patterns, and failures in an on-demand run.
- Today's satellites: an on-demand run uses the snapshot of Phases 1 to 5.
- Traffic load, congestion and packet loss, as in every earlier phase.

// Story: "Try it", the landing page. A visitor picks any two places (by name, or by clicking
// the globe) and the routing program, compiled to WebAssembly, works out in their browser how
// a message travels between them through the satellites (engine/client.ts runs it in a Web
// Worker; engine/from-csv.ts turns its output into the route story's directions). Until then,
// and while it works, the saved run of the route story's first pair plays. Everything below
// the form is a RouteView; the numbers shown are the engine's, looked up at the clock's time.

import type { CommonData } from '../../data/common';
import { parseLinks } from '../../data/common';
import { DataError } from '../../data/check';
import { parsePlaces, type Place } from '../../data/places';
import { EngineClient, EngineFailure, engineSupported, type EnginePhase } from '../../engine/client';
import { directionsFromCsv, EngineOutputError, type Lookups } from '../../engine/from-csv';
import { utcSeconds, type EngineResult } from '../../engine/run';
import { dot } from '../../theme';
import { frameLoop, segmented } from '../../ui/controls';
import { fillFold, introMarkup, legendList } from '../../ui/intro';
import { chosenFrom, placeIndex, PlaceBox, type Chosen, type PlaceIndex } from '../../ui/place-search';
import { parseRouteStory, type Direction } from '../route/data';
import { PICKED, RouteView, STRATEGY_NOTES, strategySwatch, strategyTitle, type RouteRun } from '../route/view';
import type { Story, StoryContext } from '../story';
import {
  decodeTrip,
  encodeTrip,
  headlineAt,
  nearestPlace,
  pairProblem,
  pairProblemText,
  pointName,
  runWindow,
  windowWords,
  type Trip,
  type TripPlace,
} from './state';

const ID = 'explore';

export const exploreStory: Story = {
  id: ID,
  title: 'Try it',
  summary: 'Pick any two places on Earth and see how a message would travel between them through the satellites.',
  group: 'try',
  mount,
};

/** One-tap examples: names looked up in places.json (its largest place of that name). */
const EXAMPLES: readonly (readonly [string, string])[] = [
  ['Lagos', 'Sydney'],
  ['Reykjavík', 'Tokyo'],
  ['Buenos Aires', 'Cape Town'],
];

/**
 * What the browser test reads of the last run computed here (not a visitor feature): the
 * section's data-run is a digest of the engine's routes CSV, and window.__satsimExplore holds
 * that run's output and request, so the test can compare what the page shows with the CSV the
 * worker returned.
 */
interface TestHook {
  last: (EngineResult & { request: { from: TripPlace; to: TripPlace; start_utc: string; duration_s: number } }) | null;
}
declare global {
  interface Window {
    __satsimExplore?: TestHook;
  }
}

/** A short digest (FNV-1a, 32 bits) of a text, for data-run. */
function digest(text: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < text.length; i++) {
    h ^= text.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return (h >>> 0).toString(16).padStart(8, '0');
}

interface Shown {
  directions: Direction[];
  trip: Trip;
  /** 'saved' for the route story's run, 'computed' for one worked out here. */
  origin: 'saved' | 'computed';
  run: Omit<RouteRun, 'direction' | 'from' | 'to'>;
}

async function mount({ data, root }: StoryContext): Promise<() => void> {
  const { source } = data;
  const { manifest } = source;
  const engine = manifest.engine;
  const [story, links, places] = await Promise.all([
    source.json('story-route.json', (v, c) => parseRouteStory(v, c, manifest.satellites, data.sites, manifest.settings.window_s)),
    source.json(`links-${engine.settings.pattern}.json`, (v, c) => parseLinks(v, c, manifest.satellites)),
    source.json('places.json', parsePlaces).then(
      (list): { list: Place[]; error: null } => ({ list, error: null }),
      (error: unknown): { list: Place[]; error: unknown } => ({ list: [], error }),
    ),
  ]);
  const placeList = places.list;
  const index: PlaceIndex = placeIndex(placeList);
  const strategies = engine.strategies.filter((s) => story.settings.strategies.includes(s));
  const span = runWindow(engine);

  root.innerHTML = `
    <section class="story explore" data-run-state="saved">
      ${introMarkup('Send a message through space')}
      <div class="trip-result">
        <p class="headline" aria-live="polite"></p>
        <p class="run-note"></p>
      </div>
      <div class="view-host"></div>
    </section>`;
  const el = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const section = el('.story');
  const headline = el('.headline');
  const runNote = el('.run-note');

  // The intro in two columns: the title and the words on the left, the form on the right.
  const lead = document.createElement('div');
  lead.className = 'intro-lead';
  lead.append(el('.intro h1'), el('.intro .lede'), el('.intro .try'));
  el('.intro').prepend(lead);

  // The form: From, swap, To, the button; under them the examples, or the progress of a run,
  // or what went wrong (in the same place, so the page does not jump).
  const form = document.createElement('form');
  form.className = 'trip';
  form.noValidate = true;
  form.setAttribute('aria-label', 'Choose two places');
  const view = new RouteView(el('.view-host'), data, links);
  view.addLegendItem(dot(PICKED), 'A place you chose, before you press Show the route');

  let active: PlaceBox;
  const boxes: PlaceBox[] = [];
  const events = {
    onChoose: (box: PlaceBox) => {
      setActive(box === boxes[0] ? (boxes[1] as PlaceBox) : (boxes[0] as PlaceBox));
      choiceChanged();
    },
    onFocus: (box: PlaceBox) => setActive(box),
    onSubmit: () => form.requestSubmit(),
  };
  const fromBox = new PlaceBox('From', index, events);
  const toBox = new PlaceBox('To', index, events);
  boxes.push(fromBox, toBox);
  active = fromBox;
  const swap = document.createElement('button');
  swap.type = 'button';
  swap.className = 'trip-swap';
  swap.textContent = '⇄';
  swap.title = 'Swap From and To';
  swap.setAttribute('aria-label', 'Swap From and To');
  const go = document.createElement('button');
  go.type = 'submit';
  go.className = 'trip-go';
  go.textContent = 'Show the route';
  const fields = document.createElement('div');
  fields.className = 'trip-fields';
  fields.append(fromBox.element, swap, toBox.element);
  const examples = document.createElement('p');
  examples.className = 'trip-examples';
  const examplesLabel = document.createElement('span');
  examplesLabel.textContent = 'Or try:';
  examples.append(examplesLabel);
  const status = document.createElement('div');
  status.className = 'trip-status';
  status.hidden = true;
  status.innerHTML = `
    <p class="trip-message"></p>
    <div class="trip-progress" role="progressbar" aria-label="Progress" aria-valuemin="0" aria-valuemax="100" hidden><div class="trip-bar"></div></div>
    <button type="button" class="trip-retry" hidden>Try again</button>`;
  const message = status.querySelector('.trip-message') as HTMLElement;
  const progress = status.querySelector('.trip-progress') as HTMLElement;
  const bar = status.querySelector('.trip-bar') as HTMLElement;
  const retry = status.querySelector('.trip-retry') as HTMLElement;
  const actions = document.createElement('div');
  actions.className = 'trip-actions';
  actions.append(go, examples, status);
  form.append(fields, actions);
  el('.intro-text').prepend(form);

  const globeHint = document.createElement('p');
  globeHint.className = 'pick-hint';
  globeHint.setAttribute('aria-hidden', 'true');
  el('.stage-globe').append(globeHint);

  function setActive(box: PlaceBox): void {
    active = box;
    for (const b of boxes) b.setActive(b === box);
    globeHint.textContent = `Click the globe to set ${box === fromBox ? 'From' : 'To'}`;
  }
  setActive(fromBox);

  writeText(root, strategies, span);

  // ---- The run on screen, its direction and the headline. ----
  let shown: Shown | null = null;
  let dirIndex = 0;
  const dirHost = document.createElement('div');
  view.controls.append(dirHost);

  function showRun(next: Shown, play: boolean): void {
    shown = next;
    dirIndex = 0;
    showDirection(play);
    runNote.textContent =
      next.origin === 'saved'
        ? `This first route, ${next.trip.from.name} to ${next.trip.to.name}, was worked out in advance. Choose two places and press ` +
          `Show the route to have your browser work out your own.`
        : `Worked out just now in your browser by the simulator’s own engine, for ${windowWords(span.start_utc, next.run.settings.duration_s)}, ` +
          `with the satellites where they really were then.`;
  }

  function showDirection(play: boolean): void {
    if (shown === null) return;
    const s = shown;
    const ends = [s.trip.from, s.trip.to] as const;
    const menu = segmented(
      'Direction',
      s.directions.map((_, i) => ({ value: String(i), text: `${(ends[i] ?? ends[0]).name} to ${(ends[1 - i] ?? ends[1]).name}` })),
      (i) => {
        dirIndex = i;
        showDirection(true);
      },
    );
    menu.select(dirIndex);
    dirHost.replaceChildren(menu.element);
    const from = dirIndex === 0 ? s.trip.from : s.trip.to;
    const to = dirIndex === 0 ? s.trip.to : s.trip.from;
    view.show({ ...s.run, direction: s.directions[dirIndex] as Direction, from, to }, { strategies, play });
    section.dataset.from = from.name;
    section.dataset.to = to.name;
    lastHeadline = '';
    markChosen();
  }

  let lastHeadline = '';
  const stopHeadline = frameLoop(() => {
    const run = view.run;
    const clock = view.clock;
    if (run === null || clock === null) return;
    const h = headlineAt(run.direction, { from: run.from.name, to: run.to.name }, clock.t);
    // Redrawn when anything shown or written changes. The text alone is not enough: it is
    // rounded, and the exact delay beside it (data-delay-ms) changes every second.
    const shown = JSON.stringify(h);
    if (shown === lastHeadline) return;
    lastHeadline = shown;
    headline.textContent = h.text;
    headline.dataset.state = h.kind;
    headline.dataset.delayMs = h.kind === 'route' ? String(h.delay_ms) : '';
    headline.dataset.satellites = h.kind === 'route' ? String(h.satellites) : '';
    headline.dataset.cause = h.kind === 'drop' || h.kind === 'never' ? h.cause : '';
  });

  // ---- The saved run first: the route story's first pair, the on-demand rules. ----
  const firstDirection = story.directions[0] as Direction;
  const back = story.directions.find((d) => d.from === firstDirection.to && d.to === firstDirection.from);
  const site = (i: number): TripPlace => {
    const s = data.sites[i];
    return { name: s?.name ?? '', lat_deg: s?.lat_deg ?? 0, lon_deg: s?.lon_deg ?? 0 };
  };
  const savedTrip: Trip = { from: site(firstDirection.from), to: site(firstDirection.to) };
  const windowStart = utcSeconds(manifest.settings.window_start_utc) ?? 0;
  const runSettings = (start_utc: string, duration_s: number): Shown['run'] => ({
    settings: { strategies, duration_s, step_s: engine.settings.step_s, replan_s: engine.settings.replan_s, pattern: engine.settings.pattern },
    run_start_s: (utcSeconds(start_utc) ?? windowStart) - windowStart,
  });
  const saved: Shown = {
    directions: back === undefined ? [firstDirection] : [firstDirection, back],
    trip: savedTrip,
    origin: 'saved',
    run: { ...runSettings(story.settings.start_utc, story.settings.duration_s), run_start_s: story.run_start_s },
  };

  // ---- The boxes. ----
  /** A place of a trip as a box shows it: a listed place of that name nearby gets its label. */
  const chosenOf = (p: TripPlace): Chosen => {
    const listed = placeList.find((q) => q.name === p.name && Math.abs(q.lat_deg - p.lat_deg) < 0.05 && Math.abs(q.lon_deg - p.lon_deg) < 0.05);
    return listed === undefined ? { name: p.name, label: p.name, lat_deg: p.lat_deg, lon_deg: p.lon_deg } : { ...chosenFrom(listed), lat_deg: p.lat_deg, lon_deg: p.lon_deg };
  };
  const fill = (trip: Trip): void => {
    fromBox.set(chosenOf(trip.from));
    toBox.set(chosenOf(trip.to));
  };

  /** The chosen places that are not the ends of the run on screen, as rings on the globe. */
  function markChosen(): void {
    const run = view.run;
    const same = (c: Chosen, p: TripPlace | undefined): boolean => p !== undefined && Math.abs(c.lat_deg - p.lat_deg) < 1e-6 && Math.abs(c.lon_deg - p.lon_deg) < 1e-6;
    const picked = boxes.map((b) => b.chosen).filter((c): c is Chosen => c !== null && !(same(c, run?.from) || same(c, run?.to)));
    view.markPicked(picked);
  }

  function choiceChanged(): void {
    markChosen();
    const a = fromBox.chosen;
    const b = toBox.chosen;
    if (a !== null && b !== null) view.look(a, b);
    else if (a !== null || b !== null) view.look((a ?? b) as Chosen, (a ?? b) as Chosen);
  }

  swap.addEventListener('click', () => {
    const a = fromBox.chosen;
    const b = toBox.chosen;
    fromBox.set(b);
    toBox.set(a);
    choiceChanged();
  });

  const stopPick = view.onPick((at) => {
    const near = nearestPlace(placeList, at);
    const chosen: Chosen = near === null ? { name: pointName(at.lat_deg, at.lon_deg), label: pointName(at.lat_deg, at.lon_deg), lat_deg: at.lat_deg, lon_deg: at.lon_deg } : chosenFrom(near);
    active.set(chosen, true);
  });

  for (const [a, b] of EXAMPLES) {
    const pa = placeList.find((p) => p.name === a);
    const pb = placeList.find((p) => p.name === b);
    if (pa === undefined || pb === undefined) continue;
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'trip-example';
    button.textContent = `${a} → ${b}`;
    button.addEventListener('click', () => {
      fromBox.set(chosenFrom(pa));
      toBox.set(chosenFrom(pb));
      choiceChanged();
      void compute();
    });
    examples.append(button);
  }
  const exampleCount = examples.querySelectorAll('button').length;
  examples.hidden = exampleCount === 0;

  // ---- Status: progress, a refusal, a failure. ----
  type StatusKind = 'progress' | 'notice' | 'error';
  function setStatus(kind: StatusKind | null, text = '', options: { fraction?: number; retry?: boolean } = {}): void {
    status.hidden = kind === null;
    examples.hidden = kind !== null || exampleCount === 0;
    status.dataset.kind = kind ?? '';
    message.textContent = text;
    status.setAttribute('role', kind === 'error' || kind === 'notice' ? 'alert' : 'status');
    progress.hidden = kind !== 'progress';
    const pct = Math.round(100 * Math.max(0, Math.min(1, options.fraction ?? 0)));
    bar.style.width = `${pct}%`;
    progress.setAttribute('aria-valuenow', String(pct));
    retry.hidden = options.retry !== true;
  }

  const supported = engineSupported();
  const client = supported ? new EngineClient(source) : null;
  window.__satsimExplore = { last: null };
  if (!supported) {
    go.disabled = true;
    setStatus(
      'notice',
      'This browser cannot run the simulator: it needs WebAssembly and module Web Workers, which recent versions of ' +
        'Chrome, Edge, Firefox and Safari have. The route worked out in advance still plays below.',
    );
  } else if (places.error !== null) {
    const e = places.error;
    setStatus('notice', `The list of places could not be loaded (${e instanceof DataError ? `${e.file} ${e.problem}` : e instanceof Error ? e.message : 'an error'}). Click the globe to choose places instead.`);
  }

  let running = false;
  const setRunning = (on: boolean): void => {
    running = on;
    go.textContent = on ? 'Cancel' : 'Show the route';
    go.dataset.running = String(on);
  };

  /** The place in a box, taking the best match of a half-typed name; null with a message. */
  function placeIn(box: PlaceBox, which: string): TripPlace | null {
    if (box.chosen === null) {
      const guess = box.bestGuess;
      if (guess !== null) box.set(chosenFrom(guess));
    }
    const c = box.chosen;
    if (c === null) {
      setStatus('notice', `Choose a place for ${which}: type its name and pick it from the list, or click the globe.`);
      section.dataset.runState = 'refused';
      box.input.focus();
      return null;
    }
    return { name: c.name, lat_deg: c.lat_deg, lon_deg: c.lon_deg };
  }

  let lastTrip: Trip | null = null;

  async function compute(): Promise<void> {
    if (client === null) return;
    const from = placeIn(fromBox, 'From');
    const to = from === null ? null : placeIn(toBox, 'To');
    if (from === null || to === null) return;
    const trip: Trip = { from, to };
    const problem = pairProblem(trip);
    if (problem !== null) {
      setStatus('notice', pairProblemText(trip, problem));
      section.dataset.runState = 'refused';
      return;
    }
    lastTrip = trip;
    history.replaceState(null, '', encodeTrip(ID, trip));
    markChosen();
    view.look(from, to);
    setRunning(true);
    section.dataset.runState = 'running';
    const onProgress = (phase: EnginePhase, fraction: number): void => {
      const pct = Math.round(fraction * 100);
      setStatus('progress', phase === 'loading' ? `Loading the simulator… ${pct}%` : `Working out the route… ${pct}%`, { fraction });
    };
    onProgress(client.loaded ? 'running' : 'loading', 0);
    let result: EngineResult;
    try {
      result = await client.run({ from, to, ...span, strategies }, onProgress);
    } catch (error) {
      if (error instanceof EngineFailure && error.kind === 'cancelled') return;
      setRunning(false);
      failed(error);
      return;
    }
    setRunning(false);
    if (result.exitCode !== 0) {
      failed(new EngineFailure('failed', `the simulator stopped with exit status ${result.exitCode}: ${firstLine(result.stderr)}`));
      return;
    }
    let directions: Direction[];
    try {
      directions = directionsFromCsv(result, { directions: [[result.names.from, result.names.to], [result.names.to, result.names.from]], strategies, start_utc: span.start_utc, duration_s: span.duration_s }, lookupsFor(data, result));
    } catch (error) {
      failed(error);
      return;
    }
    setStatus(null);
    window.__satsimExplore = { last: { ...result, request: { from, to, ...span } } };
    section.dataset.run = digest(result.routesCsv);
    section.dataset.runState = 'done';
    showRun({ directions, trip, origin: 'computed', run: runSettings(span.start_utc, span.duration_s) }, true);
    // On a phone the globe is below the form: bring the result into view.
    if (el('.stage-globe').getBoundingClientRect().top > innerHeight) headline.scrollIntoView({ block: 'start', behavior: 'smooth' });
  }

  function failed(error: unknown): void {
    section.dataset.runState = 'error';
    const detail = error instanceof EngineFailure || error instanceof EngineOutputError || error instanceof Error ? error.message : String(error);
    const download = error instanceof EngineFailure && error.kind === 'download';
    const text = download
      ? `The simulator could not be downloaded, so there is no new route (${detail}). Check the connection and try again.`
      : `The simulator could not work out this route (${detail}).`;
    setStatus('error', `${text} The route on screen is still the one before.`, { retry: true });
  }

  form.addEventListener('submit', (e) => {
    e.preventDefault();
    if (running) {
      client?.cancel();
      setRunning(false);
      setStatus(null);
      section.dataset.runState = 'cancelled';
      return;
    }
    void compute();
  });
  retry.addEventListener('click', () => {
    if (lastTrip !== null) fill(lastTrip);
    void compute();
  });

  // ---- Start: a shared link runs its choice; otherwise the saved run plays at once. ----
  showRun(saved, true);
  const linked = decodeTrip(location.hash);
  if (linked !== null) {
    fill(linked);
    choiceChanged();
    if (client !== null) void compute();
  } else {
    fill(savedTrip);
    markChosen();
  }

  return () => {
    stopHeadline();
    stopPick();
    client?.dispose();
    view.dispose();
    root.replaceChildren();
  };
}

/** Catalogue numbers and the run's names to the exported files' indices; the two places get
 * numbers after the saved sites (the view only compares them with the direction's ends). */
function lookupsFor(data: CommonData, result: EngineResult): Lookups {
  const satellites = new Map(data.satellites.norad.map((n, i) => [n, i]));
  const gateways = new Map(data.sites.flatMap((s, i) => (s.kind === 'gateway' ? [[s.name, i] as [string, number]] : [])));
  const n = data.sites.length;
  return {
    satellite: (norad) => satellites.get(norad),
    city: (name) => (name === result.names.from ? n : name === result.names.to ? n + 1 : undefined),
    gateway: (name) => gateways.get(name),
  };
}

function firstLine(text: string): string {
  return text.split('\n').find((l) => l.trim() !== '')?.trim() ?? 'no message';
}

/** The page's words: what this is, what to try, how to read it, what is assumed. */
function writeText(root: HTMLElement, strategies: readonly string[], span: { start_utc: string; duration_s: number }): void {
  const q = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  q('.lede').textContent =
    'A simulator of the Starlink satellite network, with its satellites at their real, published positions on one day. ' +
    'Pick two places on Earth and see the path a message takes from satellite to satellite, and how long it takes.';
  q('.try-text').textContent =
    'Type two cities or click the globe, press Show the route, and watch the path change as the satellites move.';

  fillFold(q('.how-body'), [
    'A message goes up from a dish at the first place to a satellite it can see, jumps from satellite to satellite along laser ' +
      'links, and comes down to a dish at the second place. The satellites move fast (a lap of the Earth takes about an hour and a half), ' +
      'so the path keeps changing.',
    `There are several ways to choose the path. Each rule has its own line, the same on the globe, beside it and in the charts:`,
    legendList(
      strategies.map((s) => [strategySwatch(s), `${strategyTitle(s)}: ${STRATEGY_NOTES[s] ?? ''}`.replace(/: $/, '')] as const),
      'legend-list fold-legend',
    ),
    'The big line at the top follows “Always the fastest”. Travel time is how long a message takes to get there, one way, in ' +
      'thousandths of a second (ms). When a rule has no connection, its last path is drawn dotted and dimmer, and a cross marks what broke.',
    'Drag the globe to turn it; scroll or pinch to zoom. A short click or tap (without dragging) sets a place.',
  ]);

  fillFold(q('.sources-body'), [
    'This is the best case. A dish at each place may use any satellite it can see, and the whole trip stays in space. Real ' +
      'Starlink service usually comes down to a ground station on the way and continues over the internet; the saved run ' +
      '“When a satellite breaks” shows that kind of route.',
    `The satellites and their positions are those of the day of the snapshot, not today: ${windowWords(span.start_utc, span.duration_s)}. ` +
      'The laser links follow the layout in which each satellite has three laser partners: the one ahead, the one behind, and one beside it.',
    `Three of the four rules of the simulator are offered here. The fourth, “${strategyTitle('longest-lived')}”, has to look 15 minutes ` +
      'ahead and needs more memory than a browser page should use; see it in the saved run “A connection over time”.',
    'Travel time is the time light takes along the path, nothing else: no time inside the satellites or the dishes, no queues.',
    'The simulator in your browser is the same code as the tested one, compiled to WebAssembly. Each update of this site checks ' +
      'that it gives exactly the same routes as the original program. Place names: Natural Earth populated places (public domain).',
  ]);
}

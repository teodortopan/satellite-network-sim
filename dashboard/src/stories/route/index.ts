// Story: a route through time (spec 4.6). Replays a Phase 3 `routing` run on the best-case
// model: for one direction of a study pair, the route each chosen strategy holds at the
// clock's time, its drops, its delay over the run and the engine's summary.

import { BufferAttribute, DynamicDrawUsage } from 'three';
import { Clock } from '../../clock/clock';
import { ClockView } from '../../clock/clock-view';
import { parseLinks, type CommonData } from '../../data/common';
import { formatCount, formatMs, formatSeconds, formatSpan } from '../../format';
import { dotGeometry, dots, type ThickPath } from '../../globe/layers';
import { PathPlacer, routeNodes, siteScenePositions } from '../../globe/paths';
import { SatelliteField } from '../../globe/satellite-field';
import { rgbArray, Stage } from '../../globe/stage';
import { strategyColor, THEME } from '../../theme';
import { dropdown, frameLoop, segmented, toggle } from '../../ui/controls';
import type { Story, StoryContext } from '../story';
import { DelayCharts } from './chart';
import { parseRouteStory, type Direction, type Drop, type DropElement, type RouteStory, type StrategyRun } from './data';
import { stateAt, type RouteState } from './state';
import { summaryTable } from './table';

const START_SPEED = 30;
const CAMERA_DISTANCE = 3.9;
/** Widest first, so routes the strategies share show as stripes of every colour. */
const ROUTE_WIDTHS = [9, 6.5, 4.2, 2.2];
const SAT_SIZE = 2;

export const STRATEGY_TITLES: Record<string, string> = {
  'lowest-delay': 'Lowest delay',
  'keep-until-broken': 'Keep until broken',
  'longest-lived': 'Longest lived',
  'threshold:2ms': 'Threshold 2 ms',
};

const STRATEGY_NOTES: Record<string, string> = {
  'lowest-delay': 'takes the fastest path at every re-plan',
  'keep-until-broken': 'keeps its route until a link of it breaks',
  'longest-lived': 'keeps the satellites above the two cities that stay in view longest',
  'threshold:2ms': 'switches only to a path faster by more than 2 ms',
};

const CAUSE_TITLES: Record<string, string> = {
  ground_link_lost: 'ground link lost',
  serving_left_view: 'serving satellite left view',
  laser_link_down: 'laser link down',
  no_path: 'no chain of lasers joins the cities',
  no_coverage: 'no satellite in view',
  no_gateway: 'no gateway reachable',
  no_service: 'no service',
};

export const routeStory: Story = {
  id: 'route',
  title: 'A route through time',
  mount,
};

/** The pairs of the story file: each unordered pair once, with its two directions. */
function pairsOf(story: RouteStory): { a: number; b: number; directions: Direction[] }[] {
  const pairs: { a: number; b: number; directions: Direction[] }[] = [];
  for (const d of story.directions) {
    const pair = pairs.find((p) => (p.a === d.from && p.b === d.to) || (p.a === d.to && p.b === d.from));
    if (pair === undefined) pairs.push({ a: d.from, b: d.to, directions: [d] });
    else pair.directions.push(d);
  }
  return pairs;
}

async function mount({ data, root }: StoryContext): Promise<() => void> {
  const { manifest } = data.source;
  const story = await data.source.json('story-route.json', (v, c) =>
    parseRouteStory(v, c, manifest.satellites, data.sites, manifest.settings.window_s),
  );
  const links = await data.source.json(`links-${story.settings.pattern}.json`, (v, c) => parseLinks(v, c, manifest.satellites));
  const strategies = story.settings.strategies;
  const name = (i: number): string => data.sites[i]?.name ?? '';

  root.innerHTML = `
    <section class="story route">
      <div class="intro">
        <h1>A route through time</h1>
        <div class="intro-text">
          <p class="lede"></p>
          <p class="how"></p>
        </div>
      </div>
      <div class="control-bar"></div>
      <div class="stage">
        <div class="stage-globe"></div>
        <aside class="stage-side">
          <h2>At the clock’s time</h2>
          <ul class="readouts"></ul>
          <p class="stage-note">A dashed line is a dropped route: the route held until the drop,
            drawn broken for as long as the drop lasts; the red dot marks what the engine names as
            its cause.</p>
        </aside>
      </div>
      <div class="clock-dock"></div>
      <div class="panels">
        <section class="panel chart-panel">
          <h2>Delay over the run</h2>
          <p class="panel-note chart-note"></p>
          <div class="chart-host"></div>
        </section>
        <section class="panel table-panel">
          <h2>The engine’s summary</h2>
          <p class="panel-note table-note"></p>
          <div class="table-host"></div>
        </section>
      </div>
    </section>`;
  const el = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const section = el('.story');

  const pairs = pairsOf(story);
  writeText(el('.lede'), el('.how'), story, name);
  el('.chart-note').textContent =
    'One-way delay at every second, one row per strategy on the same delay scale. Red bands are ' +
    'drops (no route); ticks along the bottom are route changes. The thin line marks the clock; ' +
    'click a chart to move the clock there.';
  el('.table-note').textContent =
    `From the routing program’s summary (${story.sources.summary}) for the chosen direction. ` +
    'A route change is any switch from one route to another without a drop between them.';

  // The globe.
  const stage = new Stage(el('.stage-globe'));
  const field = new SatelliteField(data, links.pairs);
  const sites = siteScenePositions(data);
  const placer = new PathPlacer(field, sites);
  stage.add(field.segments(THEME.links, 0.08));
  stage.add(field.points(field.shellColors, new Float32Array(field.count).fill(SAT_SIZE), stage.dotMaterial()));
  const lines = new Map<string, ThickPath>();
  strategies.forEach((s, i) => lines.set(s, stage.path(strategyColor(s), ROUTE_WIDTHS[i] ?? 2)));
  const causePosition = new BufferAttribute(new Float32Array(strategies.length * 2 * 3), 3);
  causePosition.setUsage(DynamicDrawUsage);
  const causeDots = dots(
    dotGeometry(causePosition, rgbArray(Array<string>(strategies.length * 2).fill(THEME.failed)), new Float32Array(strategies.length * 2).fill(13)),
    stage.dotMaterial(),
  );
  causeDots.renderOrder = 3;
  stage.add(causeDots);
  let endDots: ReturnType<Stage['fixedDots']> | null = null;

  // Controls.
  const shown = new Set(strategies);
  let pairIndex = 0;
  let dirIndex = 0;
  const bar = el('.control-bar');
  const pairMenu = dropdown(
    'Pair',
    pairs.map((p, i) => ({ value: String(i), text: `${name(p.a)} and ${name(p.b)}` })),
    (i) => {
      pairIndex = i;
      dirIndex = 0;
      showDirection();
    },
  );
  const dirHost = document.createElement('div');
  const toggles = document.createElement('div');
  toggles.className = 'toggles';
  toggles.setAttribute('role', 'group');
  toggles.setAttribute('aria-label', 'Strategies');
  for (const s of strategies) {
    toggles.append(
      toggle({ value: s, text: STRATEGY_TITLES[s] ?? s }, true, strategyColor(s), (on) => {
        if (on) shown.add(s);
        else shown.delete(s);
        charts?.setShown(shown);
        drawn_t = Number.NaN;
      }),
    );
  }
  bar.append(pairMenu, dirHost, toggles);

  // Readouts beside the globe, one per strategy.
  const readouts = new Map<string, HTMLElement>();
  for (const s of strategies) {
    const li = document.createElement('li');
    li.dataset.strategy = s;
    li.style.setProperty('--swatch', strategyColor(s));
    li.innerHTML = '<span class="readout-name"></span><span class="readout-state"></span>';
    (li.querySelector('.readout-name') as HTMLElement).textContent = STRATEGY_TITLES[s] ?? s;
    readouts.set(s, li);
    el('.readouts').append(li);
  }

  const clock = new Clock({ duration_s: story.settings.duration_s, speed: START_SPEED });
  const clockView = new ClockView(clock, { marks: [], spans: [], holdLabel: '' });
  el('.clock-dock').append(clockView.element);

  let direction = story.directions[0] as Direction;
  let charts: DelayCharts | null = null;
  let drawn_t = Number.NaN;
  const shownBroken = new Map<string, Drop>();

  function showDirection(): void {
    const pair = pairs[pairIndex] as (typeof pairs)[number];
    direction = pair.directions[dirIndex] as Direction;
    const menu = segmented(
      'Direction',
      pair.directions.map((d, i) => ({ value: String(i), text: `${name(d.from)} to ${name(d.to)}` })),
      (i) => {
        dirIndex = i;
        showDirection();
      },
    );
    menu.select(dirIndex);
    dirHost.replaceChildren(menu.element);
    section.dataset.from = name(direction.from);
    section.dataset.to = name(direction.to);

    charts?.dispose();
    charts = new DelayCharts(strategies, direction, STRATEGY_TITLES, (t) => {
      clock.seek(t);
    });
    charts.setShown(shown);
    el('.chart-host').replaceChildren(charts.element);
    el('.table-host').replaceChildren(summaryTable(strategies, STRATEGY_TITLES, direction));

    if (endDots !== null) {
      stage.scene.remove(endDots);
      endDots.geometry.dispose();
    }
    endDots = stage.fixedDots(
      [...placer.site(direction.from), ...placer.site(direction.to)],
      [THEME.city, THEME.city],
      [9, 9],
    );
    stage.add(endDots);
    stage.globe.lookFrom([0, 1, 2].map((k) => (sites[3 * direction.from + k] as number) + (sites[3 * direction.to + k] as number)), CAMERA_DISTANCE);
    shownBroken.clear();
    drawn_t = Number.NaN;
  }
  showDirection();

  const draw = (t: number): void => {
    field.update(story.run_start_s + t);
    let causes = 0;
    for (const s of strategies) {
      const run = direction.strategies[s] as StrategyRun;
      const state = stateAt(run, t);
      const line = lines.get(s) as ThickPath;
      const on = shown.has(s);
      line.object.visible = on;
      showReadout(readouts.get(s) as HTMLElement, state, on, data);
      if (!on || state.kind === 'none') {
        line.set([]);
        continue;
      }
      if (state.kind === 'route') {
        line.setDashed(false);
        line.set(placer.points(routeNodes(direction.from, state.interval.route, direction.to)));
        shownBroken.delete(s);
        continue;
      }
      // A drop: the route held until it, broken, and the element the engine names.
      line.setDashed(true);
      line.set(state.broken === null ? [] : placer.points(routeNodes(direction.from, state.broken.route, direction.to)));
      if (shownBroken.get(s) !== state.drop) {
        shownBroken.set(s, state.drop);
        line.layDashes();
      }
      for (const p of elementPoints(state.drop.element, placer)) {
        if (causes < strategies.length * 2) causePosition.set(p, 3 * causes++);
      }
    }
    causePosition.needsUpdate = true;
    causeDots.geometry.setDrawRange(0, causes);
    charts?.setTime(t);
  };

  const stop = frameLoop((real_s) => {
    clock.tick(real_s);
    if (clock.t !== drawn_t) {
      drawn_t = clock.t;
      draw(clock.t);
    }
    clockView.update();
    stage.render();
    document.body.dataset.ready = 'true';
  });

  return () => {
    stop();
    charts?.dispose();
    clockView.dispose();
    stage.dispose();
    root.replaceChildren();
  };
}

/** Where to put the red dot of a drop's element: a satellite, both ends of a link, a site. */
function elementPoints(e: DropElement | null, placer: PathPlacer): number[][] {
  if (e === null) return [];
  const out: (number[] | null)[] =
    e.kind === 'satellite'
      ? [placer.node({ kind: 'sat', sat: e.sat })]
      : e.kind === 'link'
        ? [placer.node({ kind: 'sat', sat: e.a }), placer.node({ kind: 'sat', sat: e.b })]
        : [placer.site(e.site)];
  return out.filter((p): p is number[] => p !== null);
}

function elementName(e: DropElement | null, data: CommonData): string {
  if (e === null) return '';
  const sat = (i: number): string => data.satellites.name[i] ?? `satellite ${i}`;
  switch (e.kind) {
    case 'satellite':
      return sat(e.sat);
    case 'link':
      return `the laser link ${sat(e.a)} to ${sat(e.b)}`;
    case 'city':
    case 'gateway':
      return data.sites[e.site]?.name ?? '';
  }
}

/** The readout of one strategy, and what the browser test reads: the state, the route as
 * catalogue numbers (as the routes CSV writes it) and the drop's cause. */
function showReadout(li: HTMLElement, state: RouteState, on: boolean, data: CommonData): void {
  const text = li.querySelector('.readout-state') as HTMLElement;
  li.dataset.on = String(on);
  li.dataset.state = state.kind;
  li.dataset.step = state.step === null ? '' : String(state.step);
  if (state.kind === 'route') {
    const hops = state.interval.route.length;
    li.dataset.route = state.interval.route.map((s) => data.satellites.norad[s]).join(' ');
    li.dataset.delayMs = state.delay_ms === null ? '' : String(state.delay_ms);
    delete li.dataset.cause;
    text.textContent = `${formatMs(state.delay_ms)}, ${formatCount(hops)} ${hops === 1 ? 'satellite' : 'satellites'}`;
  } else if (state.kind === 'drop') {
    li.dataset.route = '';
    li.dataset.delayMs = '';
    li.dataset.cause = state.drop.cause;
    const what = elementName(state.drop.element, data);
    text.textContent =
      `Dropped: ${CAUSE_TITLES[state.drop.cause] ?? state.drop.cause.replaceAll('_', ' ')}` +
      `${what === '' ? '' : ` (${what})`}, for ${formatSeconds(state.drop.length_s)} from ${formatSeconds(state.drop.start_s)}`;
  } else {
    li.dataset.route = '';
    li.dataset.delayMs = '';
    delete li.dataset.cause;
    text.textContent = 'No route';
  }
}

/** The story's text: what is shown, and the main result for the first pair, from the file. */
function writeText(lede: HTMLElement, how: HTMLElement, story: RouteStory, name: (i: number) => string): void {
  const first = story.directions[0] as Direction;
  const parts = story.settings.strategies.map((s) => {
    const sum = first.strategies[s]?.summary;
    if (sum === undefined) return '';
    return `${STRATEGY_TITLES[s] ?? s} changed route ${formatCount(sum.changes)} ${sum.changes === 1 ? 'time' : 'times'}, at a median delay of ${formatMs(sum.delay_median_ms)}`;
  });
  lede.textContent =
    'A route is the chain of satellites that carries traffic from one city to another: up to a ' +
    'satellite, across laser links between satellites, down to the other city. The satellites ' +
    'move, so the route has to change. A strategy is the rule that picks the next route. ' +
    `Over the ${formatSpan(story.settings.duration_s)} replayed here, from ${name(first.from)} to ${name(first.to)}: ` +
    `${parts.filter((p) => p !== '').join('; ')}.`;
  const notes = story.settings.strategies.map((s) => `${STRATEGY_TITLES[s] ?? s} ${STRATEGY_NOTES[s] ?? ''}`.trim());
  how.textContent =
    `The best case is shown: a city can use any satellite in view, and the laser pattern is “${story.settings.pattern}”. ` +
    `${notes.join('; ')}. Routes are re-planned every ${formatSeconds(story.settings.replan_s)}. A drop is a stretch of time with no usable route; its cause is the one the engine gives. ` +
    'Each strategy keeps its colour on the globe, in the chart and in the table. Drag to turn the globe, scroll to zoom.';
}

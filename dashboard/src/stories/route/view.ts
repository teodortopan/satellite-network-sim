// One route between two places, replayed on a globe: the line each strategy (rule for choosing
// a route) holds at the clock's time, what broke when there is no connection, a readout per
// rule, the clock, the delay charts and the summary table. Used by the saved route story and
// by any page that shows a run between two points given by coordinate.
//
// The view keeps one WebGL stage for its whole life; show() swaps in another run (another
// direction, another pair, or a run computed in the page) without creating a new one.

import { BufferAttribute, DynamicDrawUsage, type Points } from 'three';
import { Clock } from '../../clock/clock';
import { ClockView } from '../../clock/clock-view';
import type { CommonData, Links } from '../../data/common';
import { formatCount, formatMs, formatSeconds } from '../../format';
import { dotGeometry, dots, shapeArray, type ThickPath } from '../../globe/layers';
import { PathPlacer, pointScenePosition, routeNodes, siteScenePositions } from '../../globe/paths';
import { SatelliteField } from '../../globe/satellite-field';
import { rgbArray, Stage } from '../../globe/stage';
import { dot, droppedColor, DOTTED, line, MARKS, STRATEGY_STYLES, strategyStyle, THEME, type Mark } from '../../theme';
import { frameLoop, toggle } from '../../ui/controls';
import { legendList } from '../../ui/intro';
import { setSwatch } from '../../ui/marks';
import { DelayCharts } from './chart';
import type { Direction, DropElement, StrategyRun } from './data';
import { stateAt, type RouteState } from './state';
import { summaryTable } from './table';

const START_SPEED = 30;
const CAMERA_DISTANCE = 3.9;
const SAT_SIZE = 2;
/** At most this many crosses for what broke (two per rule: a laser link has two ends). */
const CAUSE_CAPACITY = 16;
const LINK_OPACITY = 0.08;

/** Each rule's name, in plain words. */
export const STRATEGY_TITLES: Record<string, string> = {
  'lowest-delay': 'Always the fastest',
  'keep-until-broken': 'Keep until it breaks',
  'longest-lived': 'Stay with long-lasting satellites',
  'threshold:2ms': 'Switch only if 2 ms faster',
};

/** What each rule does, in one plain clause. */
export const STRATEGY_NOTES: Record<string, string> = {
  'lowest-delay': 'switches to the fastest path every time routes are checked',
  'keep-until-broken': 'keeps its path until a piece of it breaks',
  'longest-lived': 'picks the satellites above the two places that will stay in view the longest',
  'threshold:2ms': 'switches only to a path that is faster by more than 2 ms',
};

/** Why there is no connection, in plain words (the engine's cause names are the keys). */
export const CAUSE_TITLES: Record<string, string> = {
  ground_link_lost: 'the link between a place and its satellite was lost',
  serving_left_view: 'the satellite above a place moved out of view',
  laser_link_down: 'a laser link on the way broke',
  no_path: 'no chain of satellites joins the two places',
  no_coverage: 'no satellite is in view',
  no_gateway: 'no ground station is in reach',
  no_service: 'no service',
};

export function strategyTitle(s: string): string {
  return STRATEGY_TITLES[s] ?? s;
}

/** A place given by coordinate: the end of a route. */
export interface Endpoint {
  name: string;
  lat_deg: number;
  lon_deg: number;
}

export interface RouteRunSettings {
  strategies: readonly string[];
  duration_s: number;
  /** Seconds per step; this view reads 1 s steps. */
  step_s: number;
  replan_s: number;
  pattern: string;
}

/**
 * One direction of one run. `direction.from` and `direction.to` are the numbers its drop
 * elements of kind `city` use for the two ends (for the saved story, indexes into the sites);
 * the ends are drawn at `from` and `to`, by coordinate.
 */
export interface RouteRun {
  settings: RouteRunSettings;
  /** Where the run starts in the position window, in seconds. */
  run_start_s: number;
  direction: Direction;
  from: Endpoint;
  to: Endpoint;
}

export interface ShowOptions {
  /** The rules to show, in this order; default: all of the run's. Each must be in the run. */
  strategies?: readonly string[];
  /** Start the clock playing (default: paused). */
  play?: boolean;
  /** The laser links to draw from now on (another pattern); default: keep the ones drawn. */
  links?: Links;
}

/** A rule's swatch: its line on the globe, narrowed to fit beside a name. */
export function strategySwatch(s: string): Mark {
  const style = strategyStyle(s);
  return line({ ...style, width: style.globeWidth * 0.6 });
}

interface Shown {
  run: RouteRun;
  strategies: readonly string[];
  /** The two ends' numbers for the placer. */
  from: number;
  to: number;
  charts: DelayCharts;
  clock: Clock;
  clockView: ClockView;
}

export class RouteView {
  /** The view's root; data-from and data-to name the shown direction's ends. */
  readonly element: HTMLElement;
  /** The bar of controls; put the page's own controls (a pair menu, a direction) in here. */
  readonly controls: HTMLElement;
  private readonly toggles: HTMLElement;
  private readonly readoutList: HTMLElement;
  private readonly stage: Stage;
  private readonly field: SatelliteField;
  private readonly placer: PathPlacer;
  private readonly lines = new Map<string, ThickPath>();
  private readonly causePosition: BufferAttribute;
  private readonly causeDots: Points;
  private endDots: Points | null = null;
  private readouts = new Map<string, HTMLElement>();
  private shownSet = new Set<string>();
  private current: Shown | null = null;
  private drawn_t = Number.NaN;
  /** What each line shows now (a route's interval or a drop), to lay its dashes when it changes. */
  private readonly onLine = new Map<string, object>();
  private readonly stop: () => void;

  constructor(
    host: HTMLElement,
    private readonly data: CommonData,
    links: Links,
    /** The most laser links any later `links` option will have (default: these). */
    linkCapacity = links.pairs.length,
  ) {
    this.element = document.createElement('div');
    this.element.className = 'route-view';
    this.element.innerHTML = `
      <div class="control-bar">
        <div class="page-controls"></div>
        <div class="control-group">
          <span class="control-label">Rules for choosing a route</span>
          <div class="toggles" role="group" aria-label="Rules for choosing a route"></div>
        </div>
      </div>
      <div class="stage">
        <div class="stage-globe" role="img" aria-label="A globe with the satellites and the route of each rule"></div>
        <aside class="stage-side">
          <h2>Right now</h2>
          <ul class="readouts"></ul>
          <div class="legend-host"></div>
        </aside>
      </div>
      <div class="clock-dock"></div>
      <div class="panels">
        <section class="panel chart-panel">
          <h2>Travel time, second by second</h2>
          <p class="panel-note">How long a message takes, one way, under each rule. Grey hatching: no
            connection. Small ticks at the bottom: the route switched. Click a chart to move the clock there.</p>
          <div class="chart-host"></div>
        </section>
        <section class="panel table-panel">
          <h2>The whole run in numbers</h2>
          <p class="panel-note">One column per rule. A switch is a change from one route to another with no break between them.</p>
          <div class="table-host"></div>
        </section>
      </div>`;
    host.append(this.element);
    const el = (selector: string): HTMLElement => this.element.querySelector(selector) as HTMLElement;
    this.controls = el('.page-controls');
    this.toggles = el('.toggles');
    this.readoutList = el('.readouts');
    el('.legend-host').append(
      legendList([
        [dot(MARKS.city), 'The two places'],
        [dot({ color: MARKS.satellite.color, shape: 'disc' }), 'A satellite'],
        [line({ color: THEME.links, width: 1, dash: [] }, 0.6), 'Laser links between satellites'],
        [line({ color: droppedColor(THEME.chalk), width: 3, dash: DOTTED }), 'No connection: the last route, dotted'],
        [dot(MARKS.cause), 'What broke'],
      ]),
    );

    this.stage = new Stage(el('.stage-globe'));
    this.field = new SatelliteField(data, links.pairs, linkCapacity);
    this.placer = new PathPlacer(this.field, siteScenePositions(data));
    this.stage.add(this.field.segments(THEME.links, LINK_OPACITY));
    this.stage.add(this.field.points(this.field.shellColors, new Float32Array(this.field.count).fill(SAT_SIZE), this.stage.dotMaterial()));
    // Widest first, so routes the rules share show as nested stripes of every style.
    for (const s of Object.keys(STRATEGY_STYLES).sort((a, b) => strategyStyle(b).globeWidth - strategyStyle(a).globeWidth)) this.lineOf(s);

    this.causePosition = new BufferAttribute(new Float32Array(CAUSE_CAPACITY * 3), 3);
    this.causePosition.setUsage(DynamicDrawUsage);
    this.causeDots = dots(
      dotGeometry(
        this.causePosition,
        rgbArray(Array<string>(CAUSE_CAPACITY).fill(MARKS.cause.color)),
        new Float32Array(CAUSE_CAPACITY).fill(13),
        undefined,
        undefined,
        shapeArray(Array(CAUSE_CAPACITY).fill(MARKS.cause.shape)),
      ),
      this.stage.dotMaterial(),
    );
    this.causeDots.renderOrder = 3;
    this.causeDots.geometry.setDrawRange(0, 0);
    this.stage.add(this.causeDots);

    this.stop = frameLoop((real_s) => this.frame(real_s));
  }

  /** The clock of the shown run (null before the first show()). */
  get clock(): Clock | null {
    return this.current?.clock ?? null;
  }

  /** The run shown now. */
  get run(): RouteRun | null {
    return this.current?.run ?? null;
  }

  /** Show one direction of a run: a new clock at 0, its charts, table, readouts and lines. */
  show(run: RouteRun, options: ShowOptions = {}): void {
    if (run.settings.step_s !== 1) throw new RangeError(`a run of ${run.settings.step_s} s steps; this view reads 1 s steps`);
    const strategies = [...(options.strategies ?? run.settings.strategies)];
    for (const s of strategies) {
      if (run.direction.strategies[s] === undefined) throw new RangeError(`the run has no rule ${s}`);
    }
    if (strategies.length === 0) throw new RangeError('no rule to show');
    if (options.links !== undefined) this.field.setLinks(options.links.pairs);

    this.current?.charts.dispose();
    this.current?.clockView.dispose();

    const from = this.placer.setExtraSites([...pointScenePosition(run.from), ...pointScenePosition(run.to)]);
    const to = from + 1;
    this.element.dataset.from = run.from.name;
    this.element.dataset.to = run.to.name;

    this.shownSet = new Set(strategies);
    this.buildToggles(strategies);
    this.buildReadouts(strategies);

    const clock = new Clock({ duration_s: run.settings.duration_s, speed: START_SPEED });
    const clockView = new ClockView(clock, { marks: [], spans: [], holdLabel: '' });
    (this.element.querySelector('.clock-dock') as HTMLElement).append(clockView.element);
    const charts = new DelayCharts(strategies, run.direction, STRATEGY_TITLES, (t) => clock.seek(t));
    charts.setShown(this.shownSet);
    (this.element.querySelector('.chart-host') as HTMLElement).replaceChildren(charts.element);
    (this.element.querySelector('.table-host') as HTMLElement).replaceChildren(summaryTable(strategies, STRATEGY_TITLES, run.direction));
    this.current = { run, strategies, from, to, charts, clock, clockView };

    if (this.endDots !== null) {
      this.stage.scene.remove(this.endDots);
      this.endDots.geometry.dispose();
    }
    this.endDots = this.stage.fixedDots(
      [...this.placer.site(from), ...this.placer.site(to)],
      [MARKS.city.color, MARKS.city.color],
      [11, 11],
      [MARKS.city.shape, MARKS.city.shape],
    );
    this.stage.add(this.endDots);
    const a = this.placer.site(from);
    const b = this.placer.site(to);
    const mid = [0, 1, 2].map((k) => (a[k] as number) + (b[k] as number));
    this.stage.globe.lookFrom(Math.hypot(...mid) < 1e-6 ? a : mid, CAMERA_DISTANCE);

    this.onLine.clear();
    this.drawn_t = Number.NaN;
    if (options.play === true) clock.play();
  }

  /** Move the clock back to the run's start; play it unless `play` is false. */
  restart(play = true): void {
    const c = this.current;
    if (c === null) return;
    c.clock.seek(0);
    if (play) c.clock.play();
    else c.clock.pause();
    c.clockView.update();
  }

  dispose(): void {
    this.stop();
    this.current?.charts.dispose();
    this.current?.clockView.dispose();
    this.current = null;
    this.stage.dispose();
    this.element.remove();
  }

  private lineOf(s: string): ThickPath {
    let path = this.lines.get(s);
    if (path === undefined) {
      const style = strategyStyle(s);
      path = this.stage.path(style.color, style.globeWidth);
      this.lines.set(s, path);
    }
    return path;
  }

  private buildToggles(strategies: readonly string[]): void {
    this.toggles.replaceChildren(
      ...strategies.map((s) =>
        toggle({ value: s, text: strategyTitle(s) }, true, strategySwatch(s), (on) => {
          if (on) this.shownSet.add(s);
          else this.shownSet.delete(s);
          this.current?.charts.setShown(this.shownSet);
          this.drawn_t = Number.NaN;
        }),
      ),
    );
  }

  private buildReadouts(strategies: readonly string[]): void {
    this.readouts = new Map();
    this.readoutList.replaceChildren(
      ...strategies.map((s) => {
        const li = document.createElement('li');
        li.dataset.strategy = s;
        li.innerHTML = '<span class="readout-name"></span><span class="readout-state"></span>';
        const name = li.querySelector('.readout-name') as HTMLElement;
        name.textContent = strategyTitle(s);
        setSwatch(name, strategySwatch(s));
        this.readouts.set(s, li);
        return li;
      }),
    );
  }

  private frame(real_s: number): void {
    const c = this.current;
    if (c !== null) {
      c.clock.tick(real_s);
      if (c.clock.t !== this.drawn_t) {
        this.drawn_t = c.clock.t;
        this.draw(c, c.clock.t);
      }
      c.clockView.update();
    }
    this.stage.render();
    document.body.dataset.ready = 'true';
  }

  private draw(c: Shown, t: number): void {
    const { run } = c;
    this.field.update(run.run_start_s + t);
    for (const [s, path] of this.lines) {
      if (!c.strategies.includes(s)) {
        path.object.visible = false;
        path.set([]);
      }
    }
    let causes = 0;
    for (const s of c.strategies) {
      const state = stateAt(run.direction.strategies[s] as StrategyRun, t);
      const path = this.lineOf(s);
      const on = this.shownSet.has(s);
      path.object.visible = on;
      showReadout(this.readouts.get(s) as HTMLElement, state, on, this.data.satellites.norad, (e) => this.elementName(c, e));
      if (!on || state.kind === 'none') {
        path.set([]);
        continue;
      }
      const style = strategyStyle(s);
      if (state.kind === 'route') {
        path.setStyle(style);
        path.set(this.placer.points(routeNodes(c.from, state.interval.route, c.to)));
        if (this.onLine.get(s) !== state.interval) {
          this.onLine.set(s, state.interval);
          path.layDashes();
        }
        continue;
      }
      // No connection: the route held until it, dotted and dimmer, and what the engine says broke.
      path.setStyle({ color: droppedColor(style.color), dash: DOTTED });
      path.set(state.broken === null ? [] : this.placer.points(routeNodes(c.from, state.broken.route, c.to)));
      if (this.onLine.get(s) !== state.drop) {
        this.onLine.set(s, state.drop);
        path.layDashes();
      }
      for (const p of this.elementPoints(c, state.drop.element)) {
        if (causes < CAUSE_CAPACITY) this.causePosition.set(p, 3 * causes++);
      }
    }
    this.causePosition.needsUpdate = true;
    this.causeDots.geometry.setDrawRange(0, causes);
    c.charts.setTime(t);
  }

  /** The placer's number of a `city` drop element: one of the two ends. */
  private endOf(c: Shown, site: number): number | null {
    if (site === c.run.direction.from) return c.from;
    if (site === c.run.direction.to) return c.to;
    return null;
  }

  /** Where to put the cross of what broke: a satellite, both ends of a link, a place. */
  private elementPoints(c: Shown, e: DropElement | null): number[][] {
    if (e === null) return [];
    const p = this.placer;
    let out: (number[] | null)[];
    if (e.kind === 'satellite') out = [p.node({ kind: 'sat', sat: e.sat })];
    else if (e.kind === 'link') out = [p.node({ kind: 'sat', sat: e.a }), p.node({ kind: 'sat', sat: e.b })];
    else if (e.kind === 'city') {
      const end = this.endOf(c, e.site);
      out = [end === null ? null : p.site(end)];
    } else out = [e.site < p.siteCount ? p.site(e.site) : null];
    return out.filter((q): q is number[] => q !== null);
  }

  private elementName(c: Shown, e: DropElement | null): string {
    if (e === null) return '';
    const sat = (i: number): string => this.data.satellites.name[i] ?? `satellite ${i}`;
    switch (e.kind) {
      case 'satellite':
        return sat(e.sat);
      case 'link':
        return `the laser link between ${sat(e.a)} and ${sat(e.b)}`;
      case 'city':
        return e.site === c.run.direction.from ? c.run.from.name : e.site === c.run.direction.to ? c.run.to.name : '';
      case 'gateway':
        return `ground station ${this.data.sites[e.site]?.name ?? ''}`.trim();
    }
  }
}

/** What a rule's readout says about its state, in plain words. */
export function stateText(state: RouteState, name: (e: DropElement | null) => string): string {
  if (state.kind === 'route') {
    const hops = state.interval.route.length;
    return `${formatMs(state.delay_ms)} one way, through ${formatCount(hops)} ${hops === 1 ? 'satellite' : 'satellites'}`;
  }
  if (state.kind === 'drop') {
    const what = name(state.drop.element);
    const why = CAUSE_TITLES[state.drop.cause] ?? state.drop.cause.replaceAll('_', ' ');
    return (
      `No connection for ${formatSeconds(state.drop.length_s)} (from ${formatSeconds(state.drop.start_s)}): ` +
      `${why}${what === '' ? '' : ` (${what})`}`
    );
  }
  return 'No route';
}

/** The readout of one rule, and what the browser test reads: the state, the route as
 * catalogue numbers (as the routes CSV writes it) and the cause of a break. */
function showReadout(
  li: HTMLElement,
  state: RouteState,
  on: boolean,
  norad: readonly number[],
  name: (e: DropElement | null) => string,
): void {
  li.dataset.on = String(on);
  li.dataset.state = state.kind;
  li.dataset.step = state.step === null ? '' : String(state.step);
  li.dataset.route = state.kind === 'route' ? state.interval.route.map((s) => norad[s]).join(' ') : '';
  li.dataset.delayMs = state.kind === 'route' && state.delay_ms !== null ? String(state.delay_ms) : '';
  if (state.kind === 'drop') li.dataset.cause = state.drop.cause;
  else delete li.dataset.cause;
  (li.querySelector('.readout-state') as HTMLElement).textContent = stateText(state, name);
}

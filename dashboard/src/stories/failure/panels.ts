// The panels beside the lanes: the facts and when each actor learned them, the run's measures,
// and the context chart of time lost over Phase 4's main runs.

import type uPlot from 'uplot';
import { AXIS, ResponsivePlot } from '../../charts/plot';
import type { CommonData } from '../../data/common';
import { formatCount, formatInstant, formatMs, formatSeconds } from '../../format';
import { dot, MODE_MARKS, THEME, type DotShape } from '../../theme';
import { drawShape, setSwatch } from '../../ui/marks';
import { MODES, type Element, type Fact, type Measure, type Mode, type Scenario } from './data';
import { MODE_SHORT, MODE_TITLES, relayed } from './lane';
import { factAt, firstDetection } from './state';

export function elementName(e: Element, data: CommonData): string {
  const sat = (i: number): string => data.satellites.name[i] ?? `satellite ${i}`;
  switch (e.kind) {
    case 'satellite':
      return `Satellite ${sat(e.sat)}`;
    case 'link':
      return `Laser link ${sat(e.a)} to ${sat(e.b)}`;
    case 'gateway':
      return `Ground station ${data.sites[e.site]?.name ?? ''}`;
    case 'city':
      return `Dish at ${data.sites[e.site]?.name ?? ''}`;
  }
}

/** The facts list: one row per fact, its cells lit as the clock passes each actor's instant. */
export class FactsPanel {
  readonly element: HTMLElement;
  private readonly rows: { fact: Fact; row: HTMLElement }[] = [];

  constructor(scenario: Scenario, data: CommonData) {
    this.element = document.createElement('section');
    this.element.className = 'panel facts';
    this.element.innerHTML = `
      <h2>Who knew what, and when</h2>
      <p class="panel-note">Each row is something that changed. Times are seconds from the start of
        the run; a cell lights up once the clock passes it.</p>
      <div class="facts-scroll">
        <table>
          <thead><tr>
            <th scope="col">What changed</th><th scope="col">When</th><th scope="col">First noticed</th>
            <th scope="col">Control centre knew</th><th scope="col">Reports sent</th><th scope="col">News spread to</th>
          </tr></thead>
          <tbody></tbody>
        </table>
      </div>`;
    const body = this.element.querySelector('tbody') as HTMLElement;
    for (const fact of scenario.facts) {
      const row = document.createElement('tr');
      row.dataset.fact = String(fact.fact);
      const reports =
        fact.reports === null
          ? 'none'
          : `${formatCount(fact.reports)}${fact.report_hops === null ? '' : ` (${relayed(fact.report_hops)})`}`;
      const cells: [string, string][] = [
        ['subject', `${elementName(fact.subject, data)} ${fact.state === 'down' ? 'stops working' : 'works again'}${fact.old ? ' (already known)' : ''}`],
        ['happened', fact.old ? 'before the run' : formatInstant(fact.t_s)],
        ['detected', formatInstant(firstDetection(fact))],
        ['planner', formatInstant(fact.planner_known_s)],
        ['reports', reports],
        ['flood', fact.flood_nodes === null || fact.flood_nodes === 0 ? 'none' : `${formatCount(fact.flood_nodes)} places, the last at ${formatInstant(fact.flood_last_s)}`],
      ];
      for (const [key, text] of cells) {
        const td = document.createElement(key === 'subject' ? 'th' : 'td');
        if (key === 'subject') td.setAttribute('scope', 'row');
        td.dataset.cell = key;
        td.textContent = text;
        row.append(td);
      }
      body.append(row);
      this.rows.push({ fact, row });
    }
  }

  update(t: number): void {
    for (const { fact, row } of this.rows) {
      const v = factAt(fact, t);
      row.dataset.happened = String(v.happened);
      row.dataset.detected = String(v.detected);
      row.dataset.plannerKnows = String(v.plannerKnows);
      row.dataset.floodDone = String(v.floodDone);
    }
  }
}

const MEASURE_ROWS: { key: Measure; label: string; unit: 's' | 'ms' | 'count' }[] = [
  { key: 'lost_s', label: 'Time with messages lost', unit: 's' },
  { key: 'black_hole_s', label: 'of it vanishing (black hole)', unit: 's' },
  { key: 'loop_s', label: 'of it going in circles (loop)', unit: 's' },
  { key: 'restore_s', label: 'Time until messages arrive again', unit: 's' },
  { key: 'settle_s', label: 'Time until the best path is in use', unit: 's' },
  { key: 'extra_median_ms', label: 'Extra travel time, typical', unit: 'ms' },
  { key: 'extra_max_ms', label: 'Extra travel time, worst', unit: 'ms' },
  { key: 'extra_settled_ms', label: 'Extra travel time once settled', unit: 'ms' },
  { key: 'reports', label: 'Reports sent to the control centre', unit: 'count' },
  { key: 'messages', label: 'News messages sent', unit: 'count' },
];

export function measuresPanel(scenario: Scenario): HTMLElement {
  const el = document.createElement('section');
  el.className = 'panel measures';
  el.innerHTML = `
    <h2>How each way did</h2>
    <p class="panel-note"></p>
    <table>
      <thead><tr><th scope="col"><span class="visually-hidden">Measure</span></th></tr></thead>
      <tbody></tbody>
    </table>`;
  (el.querySelector('.panel-note') as HTMLElement).textContent =
    `Counted from the break at ${formatInstant(scenario.failure_s)}, against the same run with nothing broken.`;
  const head = el.querySelector('thead tr') as HTMLElement;
  for (const mode of MODES) {
    const th = document.createElement('th');
    th.scope = 'col';
    th.textContent = MODE_TITLES[mode];
    head.append(th);
  }
  const body = el.querySelector('tbody') as HTMLElement;
  for (const { key, label, unit } of MEASURE_ROWS) {
    const row = document.createElement('tr');
    if (label.startsWith('of it')) row.className = 'sub';
    const th = document.createElement('th');
    th.scope = 'row';
    th.textContent = label;
    row.append(th);
    for (const mode of MODES) {
      const value = scenario.measures[mode][key];
      const td = document.createElement('td');
      td.dataset.mode = mode;
      td.dataset.measure = key;
      td.textContent = unit === 's' ? formatSeconds(value) : unit === 'ms' ? formatMs(value) : formatCount(value);
      row.append(td);
    }
    body.append(row);
  }
  return el;
}

interface DotGroup {
  xs: number[];
  ys: number[];
  color: string;
  sizePx: number;
  shape: DotShape;
}

/**
 * Time lost per mode over Phase 4's main runs of this scenario, one dot per run on a log
 * scale, this run ringed. The dots of a mode are spread over its row only so they can be told
 * apart; the spread carries no value.
 */
export class ContextChart {
  readonly element: HTMLElement;
  private readonly plot: ResponsivePlot;

  constructor(scenario: Scenario) {
    this.element = document.createElement('section');
    this.element.className = 'panel context';
    this.element.innerHTML = `
      <h2>Compared with many other runs</h2>
      <p class="panel-note"></p>
      <div class="context-plot" role="img"></div>
      <ul class="context-legend"></ul>`;
    (this.element.querySelector('.panel-note') as HTMLElement).textContent =
      'Time with messages lost in many runs of this kind of break, one dot per run, for each way of reacting. ' +
      'The ringed dot is the run shown above. Each step along the bottom is ten times the one before.';
    const plotEl = this.element.querySelector('.context-plot') as HTMLElement;
    plotEl.setAttribute('aria-label', 'Time with messages lost for each way of reacting, over many runs');
    const legend = this.element.querySelector('.context-legend') as HTMLElement;

    const groups: DotGroup[] = MODES.map((mode, row) => {
      const rows = scenario.context.rows.filter((r) => r.reaction === mode && r.lost_s !== null);
      const own = scenario.context.rows.find((r) => r.reaction === mode && r.run === scenario.context.this_run);
      const mark = MODE_MARKS[mode] ?? { color: THEME.chalk, shape: 'disc' };
      const li = document.createElement('li');
      li.textContent = `${MODE_TITLES[mode]}: ${formatSeconds(own?.lost_s ?? null)} in the run shown`;
      setSwatch(li, dot(mark));
      legend.append(li);
      return {
        xs: rows.map((r) => r.lost_s as number),
        ys: rows.map((_, i) => 2 - row + spread(i)),
        color: mark.color,
        sizePx: 4,
        shape: mark.shape,
      };
    });
    const mine = scenario.context.rows.filter((r) => r.run === scenario.context.this_run && r.lost_s !== null);
    groups.push({
      xs: mine.map((r) => r.lost_s as number),
      ys: mine.map((r) => 2 - MODES.indexOf(r.reaction)),
      color: THEME.chalk,
      sizePx: 9,
      shape: 'ring',
    });
    const ringed = document.createElement('li');
    ringed.textContent = 'The run shown above';
    setSwatch(ringed, dot({ color: THEME.chalk, shape: 'ring' }));
    legend.append(ringed);

    const data = [null, ...groups.map((g) => [g.xs, g.ys])] as unknown as uPlot.AlignedData;
    this.plot = new ResponsivePlot(plotEl, 220, (width, height) => options(width, height, groups), data);
  }

  dispose(): void {
    this.plot.dispose();
  }
}

const DECADES = [0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000];

function options(width: number, height: number, groups: DotGroup[]): uPlot.Options {
  // Each group's dots, drawn by hand: uPlot's faceted mode leaves point drawing to the caller.
  const drawDots = (u: uPlot, seriesIdx: number): null => {
    const g = groups[seriesIdx - 1] as DotGroup;
    const r = g.sizePx * devicePixelRatio;
    g.xs.forEach((x, i) => {
      drawShape(u.ctx, g.shape, u.valToPos(x, 'x', true), u.valToPos(g.ys[i] as number, 'y', true), r, g.color);
    });
    return null;
  };
  return {
    // Faceted: an x and a y list per series. uPlot types this and `distr` below as const
    // enums, which isolated modules cannot import, so the plain numbers are used.
    // eslint-disable-next-line @typescript-eslint/no-unsafe-enum-assignment
    mode: 2,
    width,
    height,
    legend: { show: false },
    cursor: { show: false },
    series: [{}, ...groups.map(() => ({ paths: drawDots, points: { show: false } }))],
    scales: {
      // Logarithmic.
      // eslint-disable-next-line @typescript-eslint/no-unsafe-enum-assignment
      x: { time: false, distr: 3 },
      y: { range: [-0.6, 2.6] },
    },
    axes: [
      {
        ...AXIS,
        label: 'Time with messages lost',
        labelSize: 22,
        splits: (_u, _i, min, max) => DECADES.filter((v) => v >= min && v <= max),
        values: (_u, ticks) => ticks.map((v) => formatSeconds(v)),
      },
      {
        ...AXIS,
        size: 120,
        splits: () => [0, 1, 2],
        values: (_u, ticks) => ticks.map((v) => MODE_SHORT[MODES[2 - v] as Mode]),
      },
    ],
  };
}

/** A fixed spread in [-0.3, 0.3] for the i-th dot of a row (golden-ratio steps, no randomness). */
function spread(i: number): number {
  return (((i * 0.618034) % 1) - 0.5) * 0.6;
}

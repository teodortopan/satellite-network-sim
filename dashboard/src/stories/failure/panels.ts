// The panels beside the lanes: the facts and when each actor learned them, the run's measures,
// and the context chart of time lost over Phase 4's main runs.

import uPlot from 'uplot';
import 'uplot/dist/uPlot.min.css';
import type { CommonData } from '../../data/common';
import { formatCount, formatInstant, formatMs, formatSeconds } from '../../format';
import { THEME } from '../../theme';
import { MODES, type Element, type Fact, type Measure, type Mode, type Scenario } from './data';
import { MODE_TITLES } from './lane';
import { factAt, firstDetection } from './state';

export function elementName(e: Element, data: CommonData): string {
  const sat = (i: number): string => data.satellites.name[i] ?? `satellite ${i}`;
  switch (e.kind) {
    case 'satellite':
      return `Satellite ${sat(e.sat)}`;
    case 'link':
      return `Laser link ${sat(e.a)} to ${sat(e.b)}`;
    case 'gateway':
      return `Gateway ${data.sites[e.site]?.name ?? ''}`;
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
      <h2>What was known, and when</h2>
      <p class="panel-note">Each fact is something that changed. Times are seconds from the run's
        start; a cell lights up once the clock passes it.</p>
      <div class="facts-scroll">
        <table>
          <thead><tr>
            <th scope="col">Fact</th><th scope="col">Happened</th><th scope="col">First detected</th>
            <th scope="col">Planner knew</th><th scope="col">Reports</th><th scope="col">Flood reached</th>
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
          : `${formatCount(fact.reports)}${fact.report_hops === null ? '' : ` (first: ${fact.report_hops} ${fact.report_hops === 1 ? 'hop' : 'hops'})`}`;
      const cells: [string, string][] = [
        ['subject', `${elementName(fact.subject, data)} ${fact.state}${fact.old ? ' (old news)' : ''}`],
        ['happened', fact.old ? 'before the run' : formatInstant(fact.t_s)],
        ['detected', formatInstant(firstDetection(fact))],
        ['planner', formatInstant(fact.planner_known_s)],
        ['reports', reports],
        ['flood', fact.flood_nodes === null || fact.flood_nodes === 0 ? 'none' : `${formatCount(fact.flood_nodes)} nodes, last at ${formatInstant(fact.flood_last_s)}`],
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
  { key: 'lost_s', label: 'Time lost', unit: 's' },
  { key: 'black_hole_s', label: 'of it in a black hole', unit: 's' },
  { key: 'loop_s', label: 'of it in a loop', unit: 's' },
  { key: 'restore_s', label: 'Time to restore', unit: 's' },
  { key: 'settle_s', label: 'Time to settle', unit: 's' },
  { key: 'extra_median_ms', label: 'Extra delay, median', unit: 'ms' },
  { key: 'extra_max_ms', label: 'Extra delay, largest', unit: 'ms' },
  { key: 'extra_settled_ms', label: 'Extra delay once settled', unit: 'ms' },
  { key: 'reports', label: 'Reports', unit: 'count' },
  { key: 'messages', label: 'Messages', unit: 'count' },
];

export function measuresPanel(scenario: Scenario): HTMLElement {
  const el = document.createElement('section');
  el.className = 'panel measures';
  el.innerHTML = `
    <h2>Measures of this run</h2>
    <p class="panel-note"></p>
    <table>
      <thead><tr><th scope="col"><span class="visually-hidden">Measure</span></th></tr></thead>
      <tbody></tbody>
    </table>`;
  (el.querySelector('.panel-note') as HTMLElement).textContent =
    `From t_f, the failure at ${formatInstant(scenario.failure_s)}, against the run with no failure. ` +
    `Engine file: ${scenario.sources.measures}.`;
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

const MODE_COLORS: Record<Mode, string> = {
  planner: '#8fb3d9',
  'local-repair': THEME.delivered,
  flooding: THEME.news,
};

interface DotGroup {
  xs: number[];
  ys: number[];
  color: string;
  sizePx: number;
  ring: boolean;
}

/**
 * Time lost per mode over Phase 4's main runs of this scenario, one dot per run on a log
 * scale, this run ringed. The dots of a mode are spread over its row only so they can be told
 * apart; the spread carries no value.
 */
export class ContextChart {
  readonly element: HTMLElement;
  private plot: uPlot | null = null;
  private readonly resize: ResizeObserver;

  constructor(scenario: Scenario) {
    this.element = document.createElement('section');
    this.element.className = 'panel context';
    this.element.innerHTML = `
      <h2>Against every main run of Phase 4</h2>
      <p class="panel-note"></p>
      <div class="context-plot" role="img"></div>
      <ul class="context-legend"></ul>`;
    (this.element.querySelector('.panel-note') as HTMLElement).textContent =
      `Time lost in every main run of this scenario, one dot per run (${scenario.context.source}). ` +
      'The ringed dot is the run replayed here.';
    const plotEl = this.element.querySelector('.context-plot') as HTMLElement;
    plotEl.setAttribute('aria-label', 'Time lost per reaction mode over the main runs, log scale');
    const legend = this.element.querySelector('.context-legend') as HTMLElement;

    const groups: DotGroup[] = MODES.map((mode, row) => {
      const rows = scenario.context.rows.filter((r) => r.reaction === mode && r.lost_s !== null);
      const own = scenario.context.rows.find((r) => r.reaction === mode && r.run === scenario.context.this_run);
      const li = document.createElement('li');
      li.style.setProperty('--swatch', MODE_COLORS[mode]);
      li.textContent = `${MODE_TITLES[mode]}: this run ${formatSeconds(own?.lost_s ?? null)}`;
      legend.append(li);
      return {
        xs: rows.map((r) => r.lost_s as number),
        ys: rows.map((_, i) => 2 - row + spread(i)),
        color: MODE_COLORS[mode],
        sizePx: 4,
        ring: false,
      };
    });
    const mine = scenario.context.rows.filter((r) => r.run === scenario.context.this_run && r.lost_s !== null);
    groups.push({
      xs: mine.map((r) => r.lost_s as number),
      ys: mine.map((r) => 2 - MODES.indexOf(r.reaction)),
      color: THEME.chalk,
      sizePx: 7,
      ring: true,
    });

    this.resize = new ResizeObserver(() => {
      const width = Math.max(plotEl.clientWidth, 200);
      if (this.plot === null) this.plot = build(plotEl, width, groups);
      else this.plot.setSize({ width, height: this.plot.height });
    });
    this.resize.observe(plotEl);
  }

  dispose(): void {
    this.resize.disconnect();
    this.plot?.destroy();
  }
}

const DECADES = [0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000];

function build(el: HTMLElement, width: number, groups: DotGroup[]): uPlot {
  const axis = { stroke: THEME.dim, grid: { stroke: THEME.rule, width: 1 }, ticks: { stroke: THEME.rule } };
  // Each group's dots, drawn by hand: uPlot's faceted mode leaves point drawing to the caller.
  const drawDots = (u: uPlot, seriesIdx: number): null => {
    const g = groups[seriesIdx - 1] as DotGroup;
    const ctx = u.ctx;
    const r = g.sizePx * devicePixelRatio;
    ctx.save();
    ctx.lineWidth = 2 * devicePixelRatio;
    ctx.strokeStyle = g.color;
    ctx.fillStyle = g.color;
    g.xs.forEach((x, i) => {
      ctx.beginPath();
      ctx.arc(u.valToPos(x, 'x', true), u.valToPos(g.ys[i] as number, 'y', true), r, 0, 2 * Math.PI);
      if (g.ring) ctx.stroke();
      else ctx.fill();
    });
    ctx.restore();
    return null;
  };
  const options: uPlot.Options = {
    // Faceted: an x and a y list per series. uPlot types this and `distr` below as const
    // enums, which isolated modules cannot import, so the plain numbers are used.
    // eslint-disable-next-line @typescript-eslint/no-unsafe-enum-assignment
    mode: 2,
    width,
    height: 220,
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
        ...axis,
        label: 'Time lost (log scale)',
        labelSize: 22,
        splits: (_u, _i, min, max) => DECADES.filter((v) => v >= min && v <= max),
        values: (_u, ticks) => ticks.map((v) => formatSeconds(v)),
      },
      {
        ...axis,
        size: 96,
        splits: () => [0, 1, 2],
        values: (_u, ticks) => ticks.map((v) => MODE_TITLES[MODES[2 - v] as Mode]),
      },
    ],
  };
  const data = [null, ...groups.map((g) => [g.xs, g.ys])];
  return new uPlot(options, data as unknown as uPlot.AlignedData, el);
}

/** A fixed spread in [-0.3, 0.3] for the i-th dot of a row (golden-ratio steps, no randomness). */
function spread(i: number): number {
  return (((i * 0.618034) % 1) - 0.5) * 0.6;
}

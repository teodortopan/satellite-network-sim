// Delay over time, one small chart per strategy on a shared delay axis (the strategies'
// lines lie on top of each other for minutes at a time, so they are drawn side by side rather
// than overlaid). Each chart shades its drops, ticks its route changes along the bottom, and
// marks the clock's time; a click moves the clock there.

import type uPlot from 'uplot';
import { AXIS, ResponsivePlot } from '../../charts/plot';
import { formatMs } from '../../format';
import { strategyColor, THEME } from '../../theme';
import type { Direction, StrategyRun } from './data';
import { changeSteps } from './state';

const ROW_HEIGHT = 120;

interface Row {
  strategy: string;
  element: HTMLElement;
  plot: ResponsivePlot;
  marker: HTMLElement;
  readout: HTMLElement;
  run: StrategyRun;
  changes: number[];
}

export class DelayCharts {
  readonly element: HTMLElement;
  private readonly rows: Row[];
  private yRange: [number, number] = [0, 1];
  private t = 0;

  constructor(
    strategies: readonly string[],
    direction: Direction,
    names: Record<string, string>,
    private readonly onSeek: (t: number) => void,
  ) {
    this.element = document.createElement('div');
    this.element.className = 'delay-charts';
    const x = Array.from({ length: (direction.strategies[strategies[0] as string] as StrategyRun).delay_ms.length }, (_, i) => i);
    this.yRange = sharedRange(direction, strategies);
    this.rows = strategies.map((strategy) => {
      const element = document.createElement('figure');
      element.className = 'delay-row';
      element.dataset.strategy = strategy;
      element.style.setProperty('--swatch', strategyColor(strategy));
      element.innerHTML = `
        <figcaption><span class="delay-name"></span><span class="delay-readout"></span></figcaption>
        <div class="delay-plot"></div>`;
      (element.querySelector('.delay-name') as HTMLElement).textContent = names[strategy] ?? strategy;
      const plotEl = element.querySelector('.delay-plot') as HTMLElement;
      plotEl.setAttribute('role', 'img');
      plotEl.setAttribute('aria-label', `One-way delay over time, ${strategy}`);
      const run = direction.strategies[strategy] as StrategyRun;
      const row: Row = {
        strategy,
        element,
        run,
        changes: changeSteps(run.intervals),
        readout: element.querySelector('.delay-readout') as HTMLElement,
        marker: document.createElement('div'),
        plot: null as unknown as ResponsivePlot,
      };
      row.marker.className = 'delay-clock';
      row.plot = new ResponsivePlot(plotEl, ROW_HEIGHT, (w, h) => this.options(row, w, h), [x, run.delay_ms] as uPlot.AlignedData, (u) => {
        u.over.append(row.marker);
        u.over.addEventListener('click', () => {
          const idx = u.cursor.idx;
          if (idx !== null && idx !== undefined) this.onSeek(idx);
        });
        this.placeMarker(row);
      });
      return row;
    });
    this.element.append(...this.rows.map((r) => r.element));
  }

  /** Show only the chosen strategies. */
  setShown(shown: ReadonlySet<string>): void {
    for (const r of this.rows) r.element.hidden = !shown.has(r.strategy);
  }

  /** Move the clock's mark to story time t. */
  setTime(t: number): void {
    this.t = t;
    for (const r of this.rows) this.placeMarker(r);
  }

  dispose(): void {
    for (const r of this.rows) r.plot.dispose();
    this.element.remove();
  }

  private placeMarker(r: Row): void {
    const u = r.plot.plot;
    if (u === null) return;
    r.marker.style.transform = `translateX(${u.valToPos(this.t, 'x')}px)`;
  }

  private options(row: Row, width: number, height: number): uPlot.Options {
    const color = strategyColor(row.strategy);
    return {
      width,
      height,
      legend: { show: false },
      cursor: { y: false, drag: { x: false, y: false }, points: { show: false } },
      scales: { x: { time: false }, y: { range: () => this.yRange } },
      series: [{}, { stroke: color, width: 1.5, points: { show: false } }],
      axes: [
        { ...AXIS, size: 30, values: (_u, ticks) => ticks.map((v) => `${v} s`) },
        { ...AXIS, size: 64, label: 'ms', labelSize: 14, values: (_u, ticks) => ticks.map((v) => formatMs(v)) },
      ],
      hooks: {
        drawClear: [(u) => drawDrops(u, row.run)],
        draw: [(u) => drawChanges(u, row.changes, color)],
        setCursor: [
          (u) => {
            const idx = u.cursor.idx;
            row.readout.textContent =
              idx === null || idx === undefined ? '' : `at ${idx} s: ${row.run.delay_ms[idx] === null ? 'no route' : formatMs(row.run.delay_ms[idx] ?? null)}`;
          },
        ],
      },
    };
  }
}

/** The delay axis shared by every row of a direction: the lowest and highest delay shown. */
function sharedRange(direction: Direction, strategies: readonly string[]): [number, number] {
  let lo = Infinity;
  let hi = -Infinity;
  for (const s of strategies) {
    for (const d of direction.strategies[s]?.delay_ms ?? []) {
      if (d === null) continue;
      if (d < lo) lo = d;
      if (d > hi) hi = d;
    }
  }
  if (!Number.isFinite(lo)) return [0, 1];
  const pad = Math.max((hi - lo) * 0.08, 0.5);
  return [Math.max(0, lo - pad), hi + pad];
}

/** Each drop as a faint band over the plot's height, behind the line. */
function drawDrops(u: uPlot, run: StrategyRun): void {
  const ctx = u.ctx;
  const { top, height } = u.bbox;
  ctx.save();
  ctx.fillStyle = `${THEME.failed}33`;
  for (const d of run.drops) {
    const x0 = u.valToPos(d.start_s, 'x', true);
    const x1 = u.valToPos(d.start_s + d.length_s, 'x', true);
    ctx.fillRect(x0, top, Math.max(x1 - x0, devicePixelRatio), height);
  }
  ctx.restore();
}

/** A short tick along the bottom at each route change. */
function drawChanges(u: uPlot, steps: readonly number[], color: string): void {
  const ctx = u.ctx;
  const { top, height } = u.bbox;
  const len = 9 * devicePixelRatio;
  ctx.save();
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.5 * devicePixelRatio;
  ctx.beginPath();
  for (const s of steps) {
    const x = Math.round(u.valToPos(s, 'x', true)) + 0.5;
    ctx.moveTo(x, top + height);
    ctx.lineTo(x, top + height - len);
  }
  ctx.stroke();
  ctx.restore();
}

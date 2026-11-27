// What every uPlot chart of the page shares: the axis look on the black page, a chart that
// follows its element's width, and line charts (grey level, width, dashes and a dot shape on
// every point tell the lines apart) with a legend that shows each line's swatch and reads out
// the values under the cursor.

import uPlot from 'uplot';
import 'uplot/dist/uPlot.min.css';
import { line as lineMark, THEME, type DotShape } from '../theme';
import { drawShape, setSwatch } from '../ui/marks';

/** Recessive axes and grid, so the lines stand out. */
export const AXIS: uPlot.Axis = {
  stroke: THEME.dim,
  grid: { stroke: THEME.rule, width: 1 },
  ticks: { stroke: THEME.rule },
};

/** A uPlot chart built once its element has a width, and resized with it. */
export class ResponsivePlot {
  plot: uPlot | null = null;
  private readonly resize: ResizeObserver;

  constructor(
    readonly element: HTMLElement,
    private readonly height: number,
    options: (width: number, height: number) => uPlot.Options,
    data: uPlot.AlignedData,
    onBuilt?: (plot: uPlot) => void,
  ) {
    this.resize = new ResizeObserver(() => {
      const width = Math.max(element.clientWidth, 200);
      if (this.plot === null) {
        this.plot = new uPlot(options(width, this.height), data, element);
        onBuilt?.(this.plot);
      } else if (width !== this.plot.width) {
        this.plot.setSize({ width, height: this.height });
      }
    });
    this.resize.observe(element);
  }

  dispose(): void {
    this.resize.disconnect();
    this.plot?.destroy();
  }
}

export interface Line {
  label: string;
  color: string;
  /** A dash pattern in CSS pixels; solid when absent. */
  dash?: readonly number[];
  /** Line width in CSS pixels (2 when absent). */
  width?: number;
  /** The dot drawn on every point (a disc when absent). */
  marker?: DotShape;
  /** One value per x, null where the line has no point. */
  values: (number | null)[];
}

export interface LineChartSpec {
  x: number[];
  lines: Line[];
  xLabel: string;
  yLabel: string;
  height: number;
  /** Logarithmic x (the failed shares 0.1% to 30%). */
  xLog?: boolean;
  xTicks?: (min: number, max: number) => number[];
  xValue: (v: number) => string;
  yValue: (v: number) => string;
  yRange?: [number, number];
  /** Dotted vertical lines at these x values (a theoretical threshold, say), each with its
   * line's dot at the top. */
  marks?: ChartMark[];
}

export interface ChartMark {
  x: number;
  color: string;
  shape?: DotShape;
}

/** Lines over a shared x, points marked, legend below that reads the values at the cursor. */
export function lineChart(element: HTMLElement, spec: LineChartSpec): ResponsivePlot {
  const options = (width: number, height: number): uPlot.Options => ({
    width,
    height,
    legend: { show: true, live: true, markers: { show: false } },
    cursor: { drag: { x: false, y: false }, points: { size: 8 } },
    scales: {
      // 3 is uPlot's logarithmic distribution (a const enum, which isolated modules cannot import).
      // eslint-disable-next-line @typescript-eslint/no-unsafe-enum-assignment
      x: { time: false, ...(spec.xLog === true ? { distr: 3 } : {}) },
      y: spec.yRange === undefined ? {} : { range: spec.yRange },
    },
    series: [
      { label: spec.xLabel, value: (_u: uPlot, v: number | null) => (v === null ? '' : spec.xValue(v)) },
      ...spec.lines.map((line) => ({
        label: line.label,
        stroke: line.color,
        width: line.width ?? 2,
        ...(line.dash === undefined ? {} : { dash: [...line.dash] }),
        // The points are drawn by drawPoints, each line with its own shape.
        points: { show: false },
        // Blank away from the points: the legend then only names the lines.
        value: (_u: uPlot, v: number | null) => (v === null ? '' : spec.yValue(v)),
      })),
    ],
    axes: [
      {
        ...AXIS,
        label: spec.xLabel,
        labelSize: 22,
        // Every given tick is labelled (a log axis would otherwise blank those between decades).
        ...(spec.xTicks === undefined
          ? {}
          : { splits: (_u: uPlot, _i: number, min: number, max: number) => spec.xTicks?.(min, max) ?? [], filter: (_u: uPlot, splits: number[]) => splits }),
        values: (_u, ticks: (number | null)[]) => ticks.map((v) => (v === null ? '' : spec.xValue(v))),
      },
      { ...AXIS, label: spec.yLabel, labelSize: 22, size: 56, values: (_u, ticks) => ticks.map((v) => spec.yValue(v)) },
    ],
    hooks: { drawClear: [(u) => drawMarks(u, spec.marks ?? [])], draw: [(u) => drawPoints(u, spec.lines)] },
  });
  // Each legend entry starts with its line's swatch (uPlot's own colour box is hidden).
  const onBuilt = (u: uPlot): void => {
    u.root.querySelectorAll<HTMLElement>('.u-legend .u-series').forEach((row, i) => {
      const line = spec.lines[i - 1];
      const th = row.querySelector('th');
      // uPlot paints a label in its line's grey when the colour box is hidden; the swatch says
      // which line it is, so every label keeps the page's text colour.
      row.querySelector<HTMLElement>('.u-label')?.style.removeProperty('color');
      if (line !== undefined && th !== null) {
        setSwatch(th, lineMark({ color: line.color, width: line.width ?? 2, dash: line.dash ?? [], marker: line.marker ?? 'disc' }));
      }
    });
  };
  return new ResponsivePlot(element, spec.height, options, [spec.x, ...spec.lines.map((l) => l.values)] as uPlot.AlignedData, onBuilt);
}

/** Every point of every shown line, as its line's dot shape. */
function drawPoints(u: uPlot, lines: readonly Line[]): void {
  const r = 3.6 * devicePixelRatio;
  lines.forEach((line, i) => {
    if (u.series[i + 1]?.show === false) return;
    const xs = u.data[0];
    line.values.forEach((v, k) => {
      const x = xs[k];
      if (v === null || x === undefined) return;
      drawShape(u.ctx, line.marker ?? 'disc', u.valToPos(x, 'x', true), u.valToPos(v, 'y', true), r, line.color);
    });
  });
}

function drawMarks(u: uPlot, marks: readonly ChartMark[]): void {
  const ctx = u.ctx;
  const { top, height } = u.bbox;
  ctx.save();
  ctx.lineWidth = 1.5 * devicePixelRatio;
  ctx.setLineDash([2 * devicePixelRatio, 4 * devicePixelRatio]);
  for (const m of marks) {
    const x = Math.round(u.valToPos(m.x, 'x', true)) + 0.5;
    ctx.strokeStyle = m.color;
    ctx.beginPath();
    ctx.moveTo(x, top);
    ctx.lineTo(x, top + height);
    ctx.stroke();
    if (m.shape !== undefined) drawShape(ctx, m.shape, x, top + 5 * devicePixelRatio, 4 * devicePixelRatio, m.color);
  }
  ctx.restore();
}

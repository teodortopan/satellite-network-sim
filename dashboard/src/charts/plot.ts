// What every uPlot chart of the page shares: the axis look on the dark theme, a chart that
// follows its element's width, and line charts with a legend that also reads out the values
// under the cursor.

import uPlot from 'uplot';
import 'uplot/dist/uPlot.min.css';
import { THEME } from '../theme';

/** Recessive axes and grid: the data carries the colour. */
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
  dash?: number[];
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
  /** Dotted vertical lines at these x values (a theoretical threshold, say). */
  marks?: { x: number; color: string }[];
}

/** Lines over a shared x, points marked, legend below that reads the values at the cursor. */
export function lineChart(element: HTMLElement, spec: LineChartSpec): ResponsivePlot {
  const options = (width: number, height: number): uPlot.Options => ({
    width,
    height,
    legend: { show: true, live: true },
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
        width: 2,
        ...(line.dash === undefined ? {} : { dash: line.dash }),
        points: { show: true, size: 6, fill: line.color, stroke: line.color },
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
    hooks: { drawClear: [(u) => drawMarks(u, spec.marks ?? [])] },
  });
  return new ResponsivePlot(element, spec.height, options, [spec.x, ...spec.lines.map((l) => l.values)] as uPlot.AlignedData);
}

function drawMarks(u: uPlot, marks: readonly { x: number; color: string }[]): void {
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
  }
  ctx.restore();
}

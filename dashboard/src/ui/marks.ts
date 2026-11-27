// The dot shapes and line styles as drawn outside WebGL: on a chart's canvas and as the small
// swatch beside a name (a switch, a readout, a legend, a table header). One definition of each
// shape, as SVG path data, serves both; the dot shader in globe/layers.ts draws the same shapes.

import type { DotShape, Mark } from '../theme';

interface Part {
  d: string;
  /** Filled, or stroked with this width (in the units of the path). */
  stroke: number | null;
}

const circle = (cx: number, cy: number, r: number): string => `M${cx - r} ${cy}a${r} ${r} 0 1 0 ${2 * r} 0a${r} ${r} 0 1 0 ${-2 * r} 0Z`;
const poly = (points: [number, number][]): string => `M${points.map(([x, y]) => `${x} ${y}`).join('L')}Z`;

/** The parts of a shape centred on (cx, cy) that fits in a circle of radius r. */
export function shapeParts(shape: DotShape, cx: number, cy: number, r: number): Part[] {
  switch (shape) {
    case 'disc':
      return [{ d: circle(cx, cy, r), stroke: null }];
    case 'ring':
      return [{ d: circle(cx, cy, r * 0.78), stroke: r * 0.42 }];
    case 'square': {
      const h = r * 0.82;
      return [{ d: poly([[cx - h, cy - h], [cx + h, cy - h], [cx + h, cy + h], [cx - h, cy + h]]), stroke: null }];
    }
    case 'square-outline': {
      const h = r * 0.7;
      return [{ d: poly([[cx - h, cy - h], [cx + h, cy - h], [cx + h, cy + h], [cx - h, cy + h]]), stroke: r * 0.34 }];
    }
    case 'cross': {
      const h = r * 0.72;
      return [{ d: `M${cx - h} ${cy - h}L${cx + h} ${cy + h}M${cx + h} ${cy - h}L${cx - h} ${cy + h}`, stroke: r * 0.44 }];
    }
    case 'plus': {
      const h = r * 0.9;
      return [{ d: `M${cx - h} ${cy}L${cx + h} ${cy}M${cx} ${cy - h}L${cx} ${cy + h}`, stroke: r * 0.44 }];
    }
    case 'diamond':
      return [{ d: poly([[cx, cy - r], [cx + r, cy], [cx, cy + r], [cx - r, cy]]), stroke: null }];
    case 'triangle':
      return [{ d: poly([[cx, cy - r], [cx + r * 0.92, cy + r * 0.7], [cx - r * 0.92, cy + r * 0.7]]), stroke: null }];
    case 'target':
      return [
        { d: circle(cx, cy, r * 0.8), stroke: r * 0.3 },
        { d: circle(cx, cy, r * 0.34), stroke: null },
      ];
  }
}

/** Draw a shape on a 2D canvas (sizes in canvas pixels). */
export function drawShape(ctx: CanvasRenderingContext2D, shape: DotShape, x: number, y: number, r: number, color: string): void {
  ctx.save();
  ctx.fillStyle = color;
  ctx.strokeStyle = color;
  ctx.setLineDash([]);
  ctx.lineCap = 'butt';
  for (const part of shapeParts(shape, x, y, r)) {
    const path = new Path2D(part.d);
    if (part.stroke === null) ctx.fill(path);
    else {
      ctx.lineWidth = part.stroke;
      ctx.stroke(path);
    }
  }
  ctx.restore();
}

function svgParts(shape: DotShape, cx: number, cy: number, r: number, color: string): string {
  return shapeParts(shape, cx, cy, r)
    .map((p) => (p.stroke === null ? `<path d="${p.d}" fill="${color}"/>` : `<path d="${p.d}" fill="none" stroke="${color}" stroke-width="${p.stroke}"/>`))
    .join('');
}

/**
 * The small picture of a series beside its name: a piece of its line (grey, width, dashes and
 * the dot on its points, if any) or its dot. Decorative: the name next to it says the same.
 */
export function swatchElement(mark: Mark): HTMLElement {
  const span = document.createElement('span');
  span.className = `swatch swatch-${mark.kind}`;
  span.setAttribute('aria-hidden', 'true');
  if (mark.kind === 'line') {
    const w = 30;
    const h = 14;
    const width = Math.min(mark.width, 8);
    const dash = mark.dash.length === 0 ? '' : ` stroke-dasharray="${mark.dash.join(' ')}"`;
    const opacity = mark.opacity === undefined ? '' : ` stroke-opacity="${mark.opacity}"`;
    const marker = mark.marker === undefined ? '' : svgParts(mark.marker, w / 2, h / 2, 4, mark.color);
    span.innerHTML =
      `<svg width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">` +
      `<line x1="0" y1="${h / 2}" x2="${w}" y2="${h / 2}" stroke="${mark.color}" stroke-width="${width}"${dash}${opacity}/>` +
      `${marker}</svg>`;
  } else {
    const s = 14;
    span.innerHTML = `<svg width="${s}" height="${s}" viewBox="0 0 ${s} ${s}">${svgParts(mark.shape, s / 2, s / 2, 5.5, mark.color)}</svg>`;
  }
  return span;
}

/** Put the swatch of `mark` first in `host`, replacing an earlier one (null removes it). */
export function setSwatch(host: HTMLElement, mark: Mark | null): void {
  host.querySelector(':scope > .swatch')?.remove();
  if (mark !== null) host.prepend(swatchElement(mark));
}

// The page's look, one source for the WebGL drawing, the charts and the CSS (applyTheme sets
// the CSS custom properties from THEME). The page is black, white and greys only: every value
// here has equal red, green and blue. What colour used to tell apart is told apart by line
// style (solid, dashed, dotted), line width, dot shape and brightness, always with a name
// beside it (a row label, a legend, a menu).

export const THEME = {
  night: '#000000',
  panel: '#0b0b0b',
  rule: '#2e2e2e',
  chalk: '#f2f2f2',
  dim: '#a3a3a3',
  earth: '#111111',
  land: '#5c5c5c',
  links: '#7a7a7a',
  delivered: '#ffffff',
  blackHole: '#d9d9d9',
  loop: '#d9d9d9',
  baseline: '#f2f2f2',
  news: '#e0e0e0',
  failed: '#ffffff',
  city: '#ffffff',
  gateway: '#cfcfcf',
  pop: '#cfcfcf',
} as const;

/** The shapes a dot can have, drawn alike on the globe (dot shader), in charts and in swatches. */
export type DotShape = 'disc' | 'ring' | 'square' | 'square-outline' | 'cross' | 'plus' | 'diamond' | 'triangle' | 'target';

/** A shape's number in the dot shader (the `aShape` attribute). */
export const SHAPE_CODE: Record<DotShape, number> = {
  disc: 0,
  ring: 1,
  square: 2,
  'square-outline': 3,
  cross: 4,
  plus: 5,
  diamond: 6,
  triangle: 7,
  target: 8,
};

/** How one series of lines is drawn: grey level, width in CSS pixels, dash pattern in CSS
 * pixels (empty for solid), and optionally a dot shape on its points (charts). */
export interface LineStyle {
  color: string;
  width: number;
  dash: readonly number[];
  marker?: DotShape;
}

/** How one kind of dot is drawn. */
export interface DotMark {
  color: string;
  shape: DotShape;
  /** Drawn this much larger than a disc of the same layer (thin shapes need more room). */
  grow?: number;
}

/** What a swatch shows: a short piece of line, or one dot. */
export type Mark = ({ kind: 'line' } & LineStyle & { opacity?: number }) | ({ kind: 'dot' } & DotMark);

export const line = (style: LineStyle, opacity?: number): Mark => ({ kind: 'line', ...style, ...(opacity === undefined ? {} : { opacity }) });
export const dot = (mark: DotMark): Mark => ({ kind: 'dot', ...mark });

const SOLID: readonly number[] = [];
/** A dropped route: dotted, so it never looks like a strategy whose own line is dashed. */
export const DOTTED: readonly number[] = [2, 9];

// Satellites by shell. Where the shells are not the subject they stay muted and close, so the
// routes, the news and the failures stand out.

const SHELL_GREYS = ['#7d7d7d', '#6c6c6c', '#8e8e8e', '#5f5f5f'] as const;

export function shellColor(shell: number): string {
  return SHELL_GREYS[((shell % SHELL_GREYS.length) + SHELL_GREYS.length) % SHELL_GREYS.length] as string;
}

/** The shells when they are the subject (the constellation story): brightness and shape. */
const SHELL_MARKS: readonly DotMark[] = [
  { color: '#ffffff', shape: 'disc' },
  { color: '#a6a6a6', shape: 'square' },
  { color: '#e0e0e0', shape: 'triangle', grow: 1.3 },
  { color: '#c4c4c4', shape: 'plus', grow: 1.4 },
  { color: '#8c8c8c', shape: 'ring' },
];

export function shellMark(shell: number): DotMark {
  return SHELL_MARKS[((shell % SHELL_MARKS.length) + SHELL_MARKS.length) % SHELL_MARKS.length] as DotMark;
}

/** Satellites not chosen in the constellation story. */
export const FADED: DotMark = { color: '#383838', shape: 'disc' };

/** One style per routing strategy, used alike on the globe, in the chart and in the table.
 * `globeWidth` is the route's width on the globe: widest first, so routes the strategies
 * share show as nested stripes. */
export const STRATEGY_STYLES: Record<string, LineStyle & { globeWidth: number }> = {
  'lowest-delay': { color: '#ffffff', width: 2, dash: SOLID, globeWidth: 9 },
  'keep-until-broken': { color: '#8f8f8f', width: 2, dash: [14, 6], globeWidth: 6.5 },
  'longest-lived': { color: '#d6d6d6', width: 2, dash: [6, 2.5], globeWidth: 4.2 },
  // Dark enough to show as a core inside the white line it often shares a route with.
  'threshold:2ms': { color: '#686868', width: 2, dash: SOLID, globeWidth: 2.6 },
};

const OTHER_STRATEGY: LineStyle & { globeWidth: number } = { color: THEME.chalk, width: 1.5, dash: SOLID, globeWidth: 2 };

export function strategyStyle(strategy: string): LineStyle & { globeWidth: number } {
  return STRATEGY_STYLES[strategy] ?? OTHER_STRATEGY;
}

/** One style per laser pattern in the charts: brightness and a dot shape on every point (the
 * dash is left free, because the charts use it for another meaning). */
export const PATTERN_STYLES: Record<string, LineStyle> = {
  'plus-grid': { color: '#ffffff', width: 2, dash: SOLID, marker: 'disc' },
  three: { color: '#a6a6a6', width: 2, dash: SOLID, marker: 'square' },
  six: { color: '#d0d0d0', width: 2, dash: SOLID, marker: 'triangle' },
};

export function patternStyle(pattern: string): LineStyle {
  return PATTERN_STYLES[pattern] ?? { color: THEME.links, width: 2, dash: SOLID, marker: 'disc' };
}

/** One style per kind of failure in the resilience charts (dashed there means re-pointed). */
export const FAILURE_KIND_STYLES: Record<string, LineStyle> = {
  'random-satellites': { color: '#ffffff', width: 2, dash: SOLID, marker: 'disc' },
  'random-lasers': { color: '#a6a6a6', width: 2, dash: SOLID, marker: 'square' },
  attack: { color: '#d0d0d0', width: 2, dash: SOLID, marker: 'cross' },
};

export function failureKindStyle(kind: string): LineStyle {
  return FAILURE_KIND_STYLES[kind] ?? { color: THEME.chalk, width: 2, dash: SOLID, marker: 'disc' };
}

/** One dot per reaction mode in the failure story's context chart. */
export const MODE_MARKS: Record<string, DotMark> = {
  planner: { color: '#bdbdbd', shape: 'square' },
  'local-repair': { color: '#ffffff', shape: 'disc' },
  flooding: { color: '#d6d6d6', shape: 'triangle' },
};

/** The route of a failure lane in each state (no route draws no line). */
export const LANE_STATE_STYLES: Record<string, LineStyle | null> = {
  delivered: { color: THEME.delivered, width: 4.5, dash: SOLID },
  black_hole: { color: THEME.blackHole, width: 4.5, dash: [10, 6] },
  loop: { color: THEME.loop, width: 4.5, dash: DOTTED },
  no_route: null,
};

/** The faint route with nothing failed, beneath the route of a lane or a resilience pair. */
export const BASELINE_STYLE: LineStyle = { color: THEME.baseline, width: 2, dash: SOLID };

/** The marks shared by every globe. */
export const MARKS = {
  city: { color: THEME.city, shape: 'target' },
  gateway: { color: THEME.gateway, shape: 'square-outline' },
  pop: { color: THEME.pop, shape: 'diamond' },
  failed: { color: THEME.failed, shape: 'cross' },
  news: { color: THEME.news, shape: 'ring' },
  cause: { color: THEME.failed, shape: 'cross' },
  blackHole: { color: THEME.blackHole, shape: 'square-outline' },
  satellite: { color: shellColor(0), shape: 'disc' },
} as const satisfies Record<string, DotMark>;

/** A dropped route's grey: dimmer than the strategy's own, never too dim to see on black. */
export function droppedColor(hex: string): string {
  return parseInt(hex.slice(1, 3), 16) * 0.7 < 0x60 ? '#606060' : dimmed(hex, 0.7);
}

/** A grey a fraction of the way from black (0) to `hex` (1). */
export function dimmed(hex: string, fraction: number): string {
  const v = Math.round(parseInt(hex.slice(1, 3), 16) * fraction);
  const h = Math.max(0, Math.min(255, v)).toString(16).padStart(2, '0');
  return `#${h}${h}${h}`;
}

export function applyTheme(root: HTMLElement): void {
  for (const [name, value] of Object.entries(THEME)) {
    root.style.setProperty(`--${name.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`)}`, value);
  }
}

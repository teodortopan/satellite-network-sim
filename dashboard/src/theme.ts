// The page's colours, one source for the WebGL drawing and the CSS (applyTheme sets the CSS
// custom properties from these).

export const THEME = {
  night: '#0c1726',
  panel: '#13233a',
  rule: '#24384f',
  chalk: '#ece7da',
  dim: '#93a3b5',
  earth: '#0f2236',
  land: '#55809f',
  links: '#6d8fb3',
  delivered: '#62d0bd',
  blackHole: '#f4a640',
  loop: '#c88cf2',
  baseline: '#ece7da',
  news: '#f6e58d',
  failed: '#ff5c74',
  city: '#ece7da',
  gateway: '#8fb3d9',
  pop: '#b9a8e0',
} as const;

/** One colour per shell, in satellites.json order; muted so routes and news stand out. */
export const SHELL_COLORS = ['#5f8fbf', '#7fa6b8', '#8a87b8', '#a39a87', '#8c96a3'] as const;

export function shellColor(shell: number): string {
  return SHELL_COLORS[shell % SHELL_COLORS.length] as string;
}

// The colours below are steps of one categorical palette for the dark surface, checked with
// the dataviz validator (lightness band, chroma, colour-blind and normal-vision separation).
// Four hues cannot all be told apart under every colour-vision deficiency, so wherever four
// are on screen each also carries a name beside it (a row label, a legend, a menu).

/** One colour per routing strategy, used alike on the globe, in the chart and in the table. */
export const STRATEGY_COLORS: Record<string, string> = {
  'lowest-delay': '#3987e5',
  'keep-until-broken': '#d95926',
  'longest-lived': '#199e70',
  'threshold:2ms': '#c98500',
};

export function strategyColor(strategy: string): string {
  return STRATEGY_COLORS[strategy] ?? THEME.chalk;
}

/** The shells when they are the subject (the constellation story): the same order as SHELL_COLORS. */
export const SHELL_COLORS_VIVID = ['#3987e5', '#c98500', '#199e70', '#d55181', '#9085e9'] as const;

export function vividShellColor(shell: number): string {
  return SHELL_COLORS_VIVID[shell % SHELL_COLORS_VIVID.length] as string;
}

/** One colour per laser pattern: its links on the globe and its lines in the charts. */
export const PATTERN_COLORS: Record<string, string> = {
  'plus-grid': '#d55181',
  three: '#9085e9',
  six: '#0ca30c',
};

export function patternColor(pattern: string): string {
  return PATTERN_COLORS[pattern] ?? THEME.links;
}

/** One colour per kind of failure in the resilience charts. */
export const FAILURE_KIND_COLORS: Record<string, string> = {
  'random-satellites': '#3987e5',
  'random-lasers': '#199e70',
  attack: '#d95926',
};

export function applyTheme(root: HTMLElement): void {
  for (const [name, value] of Object.entries(THEME)) {
    root.style.setProperty(`--${name.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`)}`, value);
  }
}

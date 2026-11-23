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

export function applyTheme(root: HTMLElement): void {
  for (const [name, value] of Object.entries(THEME)) {
    root.style.setProperty(`--${name.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`)}`, value);
  }
}

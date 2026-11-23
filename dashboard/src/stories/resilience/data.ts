// story-resilience.json (dashboard/data-format.md), read and checked.

import type { Check, JsonObject } from '../../data/check';
import type { Site } from '../../data/common';

export type Cell = number | string | null;

/** A committed summary file, as the export copied it. */
export interface Table {
  source: string;
  columns: string[];
  rows: Cell[][];
}

export type PairState = 'delivered' | 'black_hole' | 'loop' | 'no_route';
export const PAIR_STATES: readonly PairState[] = ['delivered', 'black_hole', 'loop', 'no_route'];

export interface PairRoute {
  state: PairState;
  drop_cause: string | null;
  one_way_ms: number | null;
  route: number[];
}

export interface StepPair {
  from: number;
  to: number;
  baseline: PairRoute;
  reference: PairRoute;
}

export interface NetworkRow {
  scope: string;
  working_satellites: number;
  pieces: number;
  largest_piece: number;
  largest_piece_share: number | null;
}

export interface Step {
  name: string;
  sources: Record<string, string>;
  scenario: string;
  share: number;
  seed: number | null;
  utc: string;
  t_s: number;
  pattern: string;
  repoint: string;
  failed_satellites: number[];
  failed_links: [number, number][];
  /** Indices into links-<pattern>.json `links`. */
  planned_links_not_failed: number[];
  routes: StepPair[];
  network: NetworkRow[];
}

export interface ResilienceStory {
  charts: { recovery_summary: Table; percolation: Table };
  steps: Step[];
}

/** The columns the page's charts read; a file without one of them is refused. */
export const RECOVERY_COLUMNS = ['group', 'scenario', 'pattern', 'share', 'repoint', 'runs', 'reach_reference_mean', 'reach_reference_p2_5', 'reach_reference_p97_5'] as const;
export const PERCOLATION_COLUMNS = ['links', 'kind', 'failed_share', 'seeds', 'mean_largest_piece_share', 'threshold_failed_share'] as const;

export function parseResilienceStory(
  value: unknown,
  c: Check,
  satellites: number,
  sites: readonly Site[],
  window_s: number,
  plannedLinks: (pattern: string) => number,
): ResilienceStory {
  const root = c.object(value, '');
  const get = (o: JsonObject, key: string, path: string): unknown => c.field(o, key, path);
  const sat = (v: unknown, p: string): number => c.index(v, satellites, p, 'satellites');
  const city = (v: unknown, p: string): number => {
    const i = c.index(v, sites.length, p, 'sites');
    if (sites[i]?.kind !== 'city') c.fail(p, `is site ${i}, a ${sites[i]?.kind ?? ''}, not a city`);
    return i;
  };
  const num = (o: JsonObject, key: string, path: string): number => c.number(get(o, key, path), `${path}.${key}`);
  const str = (o: JsonObject, key: string, path: string): string => c.string(get(o, key, path), `${path}.${key}`);

  const table = (v: unknown, p: string, needed: readonly string[]): Table => {
    const o = c.object(v, p);
    const columns = c.list(get(o, 'columns', p), `${p}.columns`, (x, q) => c.string(x, q));
    for (const name of needed) if (!columns.includes(name)) c.fail(`${p}.columns`, `have no column ${name}`);
    const rows = c.list(get(o, 'rows', p), `${p}.rows`, (x, q) =>
      c.array(x, q, columns.length).map((cell, i): Cell => {
        if (cell === null || typeof cell === 'string') return cell;
        return c.number(cell, `${q}[${i}]`);
      }),
    );
    return { source: str(o, 'source', p), columns, rows };
  };

  const charts = c.object(get(root, 'charts', ''), 'charts');
  const pairRoute = (v: unknown, p: string): PairRoute => {
    const o = c.object(v, p);
    return {
      state: c.oneOf(get(o, 'state', p), PAIR_STATES, `${p}.state`),
      drop_cause: c.orNull(get(o, 'drop_cause', p), (x) => c.string(x, `${p}.drop_cause`)),
      one_way_ms: c.orNull(get(o, 'one_way_ms', p), (x) => c.number(x, `${p}.one_way_ms`)),
      route: c.list(get(o, 'route', p), `${p}.route`, sat),
    };
  };

  const steps = c.list(get(root, 'steps', ''), 'steps', (v, p) => {
    const o = c.object(v, p);
    const t_s = num(o, 't_s', p);
    if (t_s < 0 || t_s > window_s) c.fail(`${p}.t_s`, `is ${t_s} s, outside the position window`);
    const pattern = str(o, 'pattern', p);
    const planned = plannedLinks(pattern);
    const so = c.object(get(o, 'sources', p), `${p}.sources`);
    const sources: Record<string, string> = {};
    for (const [k, x] of Object.entries(so)) sources[k] = c.string(x, `${p}.sources.${k}`);
    return {
      name: str(o, 'name', p),
      sources,
      scenario: str(o, 'scenario', p),
      share: num(o, 'share', p),
      seed: c.orNull(get(o, 'seed', p), (x) => c.integer(x, `${p}.seed`)),
      utc: str(o, 'utc', p),
      t_s,
      pattern,
      repoint: str(o, 'repoint', p),
      failed_satellites: c.list(get(o, 'failed_satellites', p), `${p}.failed_satellites`, sat),
      failed_links: c.list(get(o, 'failed_links', p), `${p}.failed_links`, (x, q) => {
        const a = c.array(x, q, 2);
        return [sat(a[0], `${q}[0]`), sat(a[1], `${q}[1]`)] as [number, number];
      }),
      planned_links_not_failed: c.list(get(o, 'planned_links_not_failed', p), `${p}.planned_links_not_failed`, (x, q) =>
        c.index(x, planned, q, `planned links of ${pattern}`),
      ),
      routes: c.list(get(o, 'routes', p), `${p}.routes`, (x, q) => {
        const r = c.object(x, q);
        return {
          from: city(get(r, 'from', q), `${q}.from`),
          to: city(get(r, 'to', q), `${q}.to`),
          baseline: pairRoute(get(r, 'baseline', q), `${q}.baseline`),
          reference: pairRoute(get(r, 'reference', q), `${q}.reference`),
        };
      }),
      network: c.list(get(o, 'network', p), `${p}.network`, (x, q) => {
        const r = c.object(x, q);
        return {
          scope: str(r, 'scope', q),
          working_satellites: c.integer(get(r, 'working_satellites', q), `${q}.working_satellites`),
          pieces: c.integer(get(r, 'pieces', q), `${q}.pieces`),
          largest_piece: c.integer(get(r, 'largest_piece', q), `${q}.largest_piece`),
          largest_piece_share: c.orNull(get(r, 'largest_piece_share', q), (y) => c.number(y, `${q}.largest_piece_share`)),
        };
      }),
    };
  });
  if (steps.length === 0) c.fail('steps', 'is empty');

  return {
    charts: {
      recovery_summary: table(get(charts, 'recovery_summary', 'charts'), 'charts.recovery_summary', RECOVERY_COLUMNS),
      percolation: table(get(charts, 'percolation', 'charts'), 'charts.percolation', PERCOLATION_COLUMNS),
    },
    steps,
  };
}

/** The value of `column` in a row of `table`. */
export function cellOf(table: Table, row: readonly Cell[], column: string): Cell {
  return row[table.columns.indexOf(column)] ?? null;
}

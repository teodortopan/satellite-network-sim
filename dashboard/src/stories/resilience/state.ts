// Lookups for the resilience story: a chart line from the rows of a committed summary that
// match a filter, the planned links that did not fail as index pairs, and what each study
// pair shows after recovery. Nothing here computes a simulation value; it only picks one.

import { cellOf, type Cell, type PairRoute, type Table } from './data';

export interface Points {
  x: number[];
  y: (number | null)[];
}

/** The rows of `table` whose columns equal `match`, as (x column, y column) in file order. */
export function pointsOf(table: Table, match: Record<string, Cell>, x: string, y: string): Points {
  const out: Points = { x: [], y: [] };
  for (const row of table.rows) {
    if (!Object.entries(match).every(([k, v]) => cellOf(table, row, k) === v)) continue;
    const xv = cellOf(table, row, x);
    const yv = cellOf(table, row, y);
    if (typeof xv !== 'number') continue;
    out.x.push(xv);
    out.y.push(typeof yv === 'number' ? yv : null);
  }
  return out;
}

/** Each line's y at the chart's x values (the first line's x), null where it has none there. */
export function alignTo(x: readonly number[], points: Points): (number | null)[] {
  return x.map((v) => {
    const i = points.x.indexOf(v);
    return i < 0 ? null : (points.y[i] ?? null);
  });
}

/** The listed links of `pairs` (two satellite indices per link), as index pairs to draw. */
export function pickLinks(pairs: Uint32Array, indices: readonly number[]): Uint32Array {
  const out = new Uint32Array(indices.length * 2);
  indices.forEach((k, i) => {
    out[2 * i] = pairs[2 * k] as number;
    out[2 * i + 1] = pairs[2 * k + 1] as number;
  });
  return out;
}

/** What a pair shows after recovery: its route when delivered, else "no route". */
export function afterRecovery(r: PairRoute): { routed: boolean; route: number[] } {
  return r.state === 'delivered' ? { routed: true, route: r.route } : { routed: false, route: [] };
}

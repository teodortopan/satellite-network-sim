// story-route.json (dashboard/data-format.md), read and checked.

import type { Check, JsonObject } from '../../data/check';
import type { Site } from '../../data/common';

/** A run of steps with the same route: both ends included, in steps from the run's start. */
export interface RouteInterval {
  first_s: number;
  last_s: number;
  route: number[];
  gateway: number | null;
}

export type DropElement =
  | { kind: 'satellite'; sat: number }
  | { kind: 'link'; a: number; b: number; reason: string }
  | { kind: 'city' | 'gateway'; site: number };

export interface Drop {
  start_s: number;
  length_s: number;
  cause: string;
  element: DropElement | null;
}

export interface RouteSummary {
  ok_steps: number;
  steps: number;
  delay_median_ms: number | null;
  bad_changes: number;
  increases: number;
  breaks: number;
  recoveries: number;
  drop_events: number;
  no_route_s: Record<string, number>;
  changes: number;
  no_route_total_s: number;
}

export interface StrategyRun {
  intervals: RouteInterval[];
  /** The delay at every step (index = step), null without a route. */
  delay_ms: (number | null)[];
  drops: Drop[];
  summary: RouteSummary;
}

export interface Direction {
  from: number;
  to: number;
  strategies: Record<string, StrategyRun>;
}

export interface RouteStory {
  sources: { routes: string; drops: string; summary: string };
  settings: { model: string; pattern: string; replan_s: number; start_utc: string; duration_s: number; step_s: number; strategies: string[] };
  run_start_s: number;
  directions: Direction[];
}

const SUMMARY_COUNTS = ['ok_steps', 'steps', 'bad_changes', 'increases', 'breaks', 'recoveries', 'drop_events', 'changes'] as const;

export function parseRouteStory(
  value: unknown,
  c: Check,
  satellites: number,
  sites: readonly Site[],
  window_s: number,
): RouteStory {
  const root = c.object(value, '');
  const get = (o: JsonObject, key: string, path: string): unknown => c.field(o, key, path);
  const sat = (v: unknown, p: string): number => c.index(v, satellites, p, 'satellites');
  const site = (v: unknown, p: string, kind: Site['kind']): number => {
    const i = c.index(v, sites.length, p, 'sites');
    const s = sites[i] as Site;
    if (s.kind !== kind) c.fail(p, `is site ${i}, a ${s.kind}, not a ${kind}`);
    return i;
  };
  const str = (o: JsonObject, key: string, path: string): string => c.string(get(o, key, path), `${path}.${key}`);

  const s = c.object(get(root, 'settings', ''), 'settings');
  const settings = {
    model: str(s, 'model', 'settings'),
    pattern: str(s, 'pattern', 'settings'),
    replan_s: c.number(get(s, 'replan_s', 'settings'), 'settings.replan_s'),
    start_utc: str(s, 'start_utc', 'settings'),
    duration_s: c.integer(get(s, 'duration_s', 'settings'), 'settings.duration_s'),
    step_s: c.integer(get(s, 'step_s', 'settings'), 'settings.step_s'),
    strategies: c.list(get(s, 'strategies', 'settings'), 'settings.strategies', (v, p) => c.string(v, p)),
  };
  // Steps are seconds only at a 1 s step (data-format.md: the state at t is that of step floor(t)).
  if (settings.step_s !== 1) c.fail('settings.step_s', `is ${settings.step_s}; this page reads 1 s steps`);
  if (settings.strategies.length === 0) c.fail('settings.strategies', 'is empty');
  const run_start_s = c.number(get(root, 'run_start_s', ''), 'run_start_s');
  if (settings.duration_s <= 0 || run_start_s < 0 || run_start_s + settings.duration_s > window_s) {
    c.fail('settings.duration_s', `runs from ${run_start_s} s for ${settings.duration_s} s, outside the position window`);
  }
  const steps = settings.duration_s + 1;

  const element = (v: unknown, p: string): DropElement => {
    const a = c.array(v, p);
    const kind = c.oneOf(a[0], ['satellite', 'link', 'city', 'gateway'] as const, `${p}[0]`);
    switch (kind) {
      case 'satellite':
        return { kind, sat: sat(a[1], `${p}[1]`) };
      case 'link':
        return { kind, a: sat(a[1], `${p}[1]`), b: sat(a[2], `${p}[2]`), reason: c.string(a[3], `${p}[3]`) };
      case 'city':
      case 'gateway':
        return { kind, site: site(a[1], `${p}[1]`, kind) };
    }
  };

  const run = (v: unknown, p: string): StrategyRun => {
    const o = c.object(v, p);
    const intervals = c.list(get(o, 'intervals', p), `${p}.intervals`, (x, q) => {
      const a = c.array(x, q, 4);
      return {
        first_s: c.integer(a[0], `${q}[0]`),
        last_s: c.integer(a[1], `${q}[1]`),
        route: c.list(a[2], `${q}[2]`, sat),
        gateway: c.orNull(a[3], (g) => site(g, `${q}[3]`, 'gateway')),
      };
    });
    // In time order, apart, inside the run: the binary searches of state.ts rely on it.
    let next = 0;
    intervals.forEach((iv, i) => {
      if (iv.first_s < next || iv.last_s < iv.first_s || iv.last_s >= steps) {
        c.fail(`${p}.intervals[${i}]`, `should start at step ${next} or later, end at or after its start and before step ${steps}`);
      }
      next = iv.last_s + 1;
    });
    const delay_ms = c
      .array(get(o, 'delay_ms', p), `${p}.delay_ms`, steps)
      .map((x, i) => c.orNull(x, (d) => c.number(d, `${p}.delay_ms[${i}]`)));
    const drops = c.list(get(o, 'drops', p), `${p}.drops`, (x, q) => {
      const a = c.array(x, q, 4);
      return {
        start_s: c.number(a[0], `${q}[0]`),
        length_s: c.number(a[1], `${q}[1]`),
        cause: c.string(a[2], `${q}[2]`),
        element: c.orNull(a[3], (e) => element(e, `${q}[3]`)),
      };
    });
    let free = 0;
    drops.forEach((d, i) => {
      if (d.start_s < free || !(d.length_s > 0) || d.start_s >= steps) {
        c.fail(`${p}.drops[${i}]`, `should start at ${free} s or later, inside the run, and last a while`);
      }
      free = d.start_s + d.length_s;
    });
    const so = c.object(get(o, 'summary', p), `${p}.summary`);
    const sp = `${p}.summary`;
    const counts = {} as Record<(typeof SUMMARY_COUNTS)[number], number>;
    for (const key of SUMMARY_COUNTS) counts[key] = c.integer(get(so, key, sp), `${sp}.${key}`);
    if (counts.steps !== steps) c.fail(`${sp}.steps`, `is ${counts.steps}, but the run has ${steps} steps`);
    const causes = c.object(get(so, 'no_route_s', sp), `${sp}.no_route_s`);
    const no_route_s: Record<string, number> = {};
    for (const [cause, seconds] of Object.entries(causes)) no_route_s[cause] = c.number(seconds, `${sp}.no_route_s.${cause}`);
    return {
      intervals,
      delay_ms,
      drops,
      summary: {
        ...counts,
        delay_median_ms: c.orNull(get(so, 'delay_median_ms', sp), (x) => c.number(x, `${sp}.delay_median_ms`)),
        no_route_s,
        no_route_total_s: c.number(get(so, 'no_route_total_s', sp), `${sp}.no_route_total_s`),
      },
    };
  };

  const directions = c.list(get(root, 'directions', ''), 'directions', (v, p) => {
    const o = c.object(v, p);
    const so = c.object(get(o, 'strategies', p), `${p}.strategies`);
    const strategies: Record<string, StrategyRun> = {};
    for (const name of settings.strategies) strategies[name] = run(get(so, name, `${p}.strategies`), `${p}.strategies.${name}`);
    return { from: site(get(o, 'from', p), `${p}.from`, 'city'), to: site(get(o, 'to', p), `${p}.to`, 'city'), strategies };
  });
  if (directions.length === 0) c.fail('directions', 'is empty');

  const so = c.object(get(root, 'sources', ''), 'sources');
  return {
    sources: { routes: str(so, 'routes', 'sources'), drops: str(so, 'drops', 'sources'), summary: str(so, 'summary', 'sources') },
    settings,
    run_start_s,
    directions,
  };
}

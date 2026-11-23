// story-failure.json (dashboard/data-format.md), read and checked.

import type { Check, JsonObject } from '../../data/check';
import type { Site } from '../../data/common';

export const MODES = ['planner', 'local-repair', 'flooding'] as const;
export type Mode = (typeof MODES)[number];
export const LANES = ['baseline', 'reference', ...MODES] as const;
export type LaneName = (typeof LANES)[number];

export type LaneState = 'delivered' | 'black_hole' | 'loop' | 'no_route';
export const LANE_STATES: readonly LaneState[] = ['delivered', 'black_hole', 'loop', 'no_route'];

export type Element =
  | { kind: 'satellite'; sat: number }
  | { kind: 'link'; a: number; b: number }
  | { kind: 'gateway'; site: number }
  | { kind: 'city'; site: number };

export interface Interval {
  start_s: number;
  end_s: number;
  state: LaneState;
  drop_cause: string | null;
  failed_element: Element | null;
  one_way_ms: number | null;
  route: number[];
  serving: number | null;
  gateway: number | null;
}

export const MEASURES = [
  'failure_s',
  'black_hole_s',
  'loop_s',
  'lost_s',
  'restore_s',
  'settle_s',
  'extra_min_ms',
  'extra_median_ms',
  'extra_max_ms',
  'extra_settled_ms',
  'reports',
  'messages',
] as const;
export type Measure = (typeof MEASURES)[number];
export type Measures = Record<Measure, number | null>;

export interface Detector {
  kind: 'satellite' | 'city' | 'gateway';
  index: number;
  t_s: number;
}

export interface Fact {
  fact: number;
  subject: Element;
  state: 'down' | 'up';
  t_s: number;
  old: boolean;
  detectors: Detector[];
  planner_known_s: number | null;
  reports: number | null;
  report_hops: number | null;
  flood_nodes: number | null;
  flood_last_s: number | null;
  messages: number | null;
}

export interface Span {
  from_s: number;
  to_s: number;
  speed: number;
}

export interface ContextRow {
  run: string;
  reaction: Mode;
  lost_s: number | null;
}

export interface Scenario {
  scenario: string;
  start_utc: string;
  duration_s: number;
  run_start_s: number;
  failure_s: number;
  failed: { satellites: number[]; links: [number, number][]; gateways: number[] };
  home_pop: number;
  lanes: Record<LaneName, Interval[]>;
  plans: Record<LaneName, [number, string][]>;
  measures: Record<Mode, Measures>;
  sources: { measures: string; events: string };
  facts: Fact[];
  flood: { satellite_first_learn_s: (number | null)[]; site_first_learn_s: (number | null)[] };
  slow_motion: Span[];
  pause_before_failure_real_s: number;
  context: { source: string; this_run: string; rows: ContextRow[] };
}

export interface FailureStory {
  from: number;
  to: number;
  instant_utc: string;
  settings: JsonObject;
  scenarios: Scenario[];
}

export function parseFailureStory(
  value: unknown,
  c: Check,
  satellites: number,
  sites: readonly Site[],
  window_s: number,
): FailureStory {
  const root = c.object(value, '');
  const get = (o: JsonObject, key: string, path: string): unknown => c.field(o, key, path);
  const sat = (v: unknown, p: string): number => c.index(v, satellites, p, 'satellites');
  const site = (v: unknown, p: string, kind: Site['kind'] | null): number => {
    const i = c.index(v, sites.length, p, 'sites');
    const s = sites[i] as Site;
    if (kind !== null && s.kind !== kind) c.fail(p, `is site ${i}, a ${s.kind}, not a ${kind}`);
    return i;
  };
  const num = (o: JsonObject, key: string, path: string): number =>
    c.number(get(o, key, path), `${path}.${key}`);
  const numOrNull = (v: unknown, p: string): number | null => c.orNull(v, (x) => c.number(x, p));
  const intOrNull = (v: unknown, p: string): number | null => c.orNull(v, (x) => c.integer(x, p));

  const element = (v: unknown, p: string): Element => {
    const a = c.array(v, p);
    const kind = c.oneOf(a[0], ['satellite', 'link', 'gateway', 'city'] as const, `${p}[0]`);
    switch (kind) {
      case 'satellite':
        return { kind, sat: sat(a[1], `${p}[1]`) };
      case 'link':
        return { kind, a: sat(a[1], `${p}[1]`), b: sat(a[2], `${p}[2]`) };
      case 'gateway':
        return { kind, site: site(a[1], `${p}[1]`, 'gateway') };
      case 'city':
        return { kind, site: site(a[1], `${p}[1]`, 'city') };
    }
  };

  const interval = (v: unknown, p: string): Interval => {
    const a = c.array(v, p, 9);
    return {
      start_s: c.number(a[0], `${p}[0]`),
      end_s: c.number(a[1], `${p}[1]`),
      state: c.oneOf(a[2], LANE_STATES, `${p}[2]`),
      drop_cause: c.orNull(a[3], (x) => c.string(x, `${p}[3]`)),
      failed_element: c.orNull(a[4], (x) => element(x, `${p}[4]`)),
      one_way_ms: numOrNull(a[5], `${p}[5]`),
      route: c.list(a[6], `${p}[6]`, sat),
      serving: c.orNull(a[7], (x) => sat(x, `${p}[7]`)),
      gateway: c.orNull(a[8], (x) => site(x, `${p}[8]`, 'gateway')),
    };
  };

  const scenario = (v: unknown, p: string): Scenario => {
    const s = c.object(v, p);
    const duration_s = c.integer(get(s, 'duration_s', p), `${p}.duration_s`);
    const run_start_s = c.integer(get(s, 'run_start_s', p), `${p}.run_start_s`);
    if (duration_s <= 0 || run_start_s < 0 || run_start_s + duration_s > window_s) {
      c.fail(p, `runs from ${run_start_s} s for ${duration_s} s, outside the position window`);
    }
    const lanesObj = c.object(get(s, 'lanes', p), `${p}.lanes`);
    const plansObj = c.object(get(s, 'plans', p), `${p}.plans`);
    const lanes = {} as Record<LaneName, Interval[]>;
    const plans = {} as Record<LaneName, [number, string][]>;
    for (const lane of LANES) {
      const lp = `${p}.lanes.${lane}`;
      const intervals = c.list(get(lanesObj, lane, `${p}.lanes`), lp, interval);
      checkTiling(c, intervals, duration_s, lp);
      lanes[lane] = intervals;
      plans[lane] = c.list(get(plansObj, lane, `${p}.plans`), `${p}.plans.${lane}`, (x, q) => {
        const a = c.array(x, q, 2);
        return [c.number(a[0], `${q}[0]`), c.oneOf(a[1], ['replaced', 'kept', 'none'], `${q}[1]`)];
      });
    }
    const measuresObj = c.object(get(s, 'measures', p), `${p}.measures`);
    const measures = {} as Record<Mode, Measures>;
    for (const mode of MODES) {
      const mp = `${p}.measures.${mode}`;
      const m = c.object(get(measuresObj, mode, `${p}.measures`), mp);
      const out = {} as Measures;
      for (const key of MEASURES) {
        const raw = get(m, key, mp);
        out[key] =
          key === 'reports' || key === 'messages'
            ? intOrNull(raw, `${mp}.${key}`)
            : numOrNull(raw, `${mp}.${key}`);
      }
      measures[mode] = out;
    }
    const failedObj = c.object(get(s, 'failed', p), `${p}.failed`);
    const fp = `${p}.failed`;
    const facts = c.list(get(s, 'facts', p), `${p}.facts`, (x, q) => {
      const f = c.object(x, q);
      return {
        fact: c.integer(get(f, 'fact', q), `${q}.fact`),
        subject: element(get(f, 'subject', q), `${q}.subject`),
        state: c.oneOf(get(f, 'state', q), ['down', 'up'] as const, `${q}.state`),
        t_s: num(f, 't_s', q),
        old: c.boolean(get(f, 'old', q), `${q}.old`),
        detectors: c.list(get(f, 'detectors', q), `${q}.detectors`, (d, dp) => {
          const a = c.array(d, dp, 3);
          const kind = c.oneOf(a[0], ['satellite', 'city', 'gateway'] as const, `${dp}[0]`);
          const index =
            kind === 'satellite' ? sat(a[1], `${dp}[1]`) : site(a[1], `${dp}[1]`, kind);
          return { kind, index, t_s: c.number(a[2], `${dp}[2]`) };
        }),
        planner_known_s: numOrNull(get(f, 'planner_known_s', q), `${q}.planner_known_s`),
        reports: intOrNull(get(f, 'reports', q), `${q}.reports`),
        report_hops: intOrNull(get(f, 'report_hops', q), `${q}.report_hops`),
        flood_nodes: intOrNull(get(f, 'flood_nodes', q), `${q}.flood_nodes`),
        flood_last_s: numOrNull(get(f, 'flood_last_s', q), `${q}.flood_last_s`),
        messages: intOrNull(get(f, 'messages', q), `${q}.messages`),
      };
    });
    const floodObj = c.object(get(s, 'flood', p), `${p}.flood`);
    const learn = (key: string, length: number): (number | null)[] =>
      c
        .array(get(floodObj, key, `${p}.flood`), `${p}.flood.${key}`, length)
        .map((x, i) => numOrNull(x, `${p}.flood.${key}[${i}]`));
    const contextObj = c.object(get(s, 'context', p), `${p}.context`);
    const sourcesObj = c.object(get(s, 'sources', p), `${p}.sources`);
    return {
      scenario: c.string(get(s, 'scenario', p), `${p}.scenario`),
      start_utc: c.string(get(s, 'start_utc', p), `${p}.start_utc`),
      duration_s,
      run_start_s,
      failure_s: num(s, 'failure_s', p),
      failed: {
        satellites: c.list(get(failedObj, 'satellites', fp), `${fp}.satellites`, sat),
        links: c.list(get(failedObj, 'links', fp), `${fp}.links`, (x, q) => {
          const a = c.array(x, q, 2);
          return [sat(a[0], `${q}[0]`), sat(a[1], `${q}[1]`)];
        }),
        gateways: c.list(get(failedObj, 'gateways', fp), `${fp}.gateways`, (x, q) =>
          site(x, q, 'gateway'),
        ),
      },
      home_pop: site(get(s, 'home_pop', p), `${p}.home_pop`, 'pop'),
      lanes,
      plans,
      measures,
      sources: {
        measures: c.string(get(sourcesObj, 'measures', `${p}.sources`), `${p}.sources.measures`),
        events: c.string(get(sourcesObj, 'events', `${p}.sources`), `${p}.sources.events`),
      },
      facts,
      flood: {
        satellite_first_learn_s: learn('satellite_first_learn_s', satellites),
        site_first_learn_s: learn('site_first_learn_s', sites.length),
      },
      slow_motion: c.list(get(s, 'slow_motion', p), `${p}.slow_motion`, (x, q) => {
        const o = c.object(x, q);
        const span = { from_s: num(o, 'from_s', q), to_s: num(o, 'to_s', q), speed: num(o, 'speed', q) };
        if (!(span.to_s > span.from_s && span.speed > 0)) c.fail(q, 'is an empty span or has no speed');
        return span;
      }),
      pause_before_failure_real_s: num(s, 'pause_before_failure_real_s', p),
      context: {
        source: c.string(get(contextObj, 'source', `${p}.context`), `${p}.context.source`),
        this_run: c.string(get(contextObj, 'this_run', `${p}.context`), `${p}.context.this_run`),
        rows: c.list(get(contextObj, 'rows', `${p}.context`), `${p}.context.rows`, (x, q) => {
          const o = c.object(x, q);
          return {
            run: c.string(get(o, 'run', q), `${q}.run`),
            reaction: c.oneOf(get(o, 'reaction', q), MODES, `${q}.reaction`),
            lost_s: numOrNull(get(o, 'lost_s', q), `${q}.lost_s`),
          };
        }),
      },
    };
  };

  const scenarios = c.list(get(root, 'scenarios', ''), 'scenarios', scenario);
  if (scenarios.length === 0) c.fail('scenarios', 'is empty');
  return {
    from: site(get(root, 'from', ''), 'from', 'city'),
    to: site(get(root, 'to', ''), 'to', 'city'),
    instant_utc: c.string(get(root, 'instant_utc', ''), 'instant_utc'),
    settings: c.object(get(root, 'settings', ''), 'settings'),
    scenarios,
  };
}

/** A lane's intervals must follow each other without gaps from 0 to the run's end. */
function checkTiling(c: Check, intervals: Interval[], duration_s: number, path: string): void {
  let at = 0;
  intervals.forEach((iv, i) => {
    if (iv.start_s !== at || !(iv.end_s > iv.start_s)) {
      c.fail(`${path}[${i}]`, `should start at ${at} s and end after it`);
    }
    at = iv.end_s;
  });
  if (at !== duration_s) c.fail(path, `ends at ${at} s, not at the run's end ${duration_s} s`);
}

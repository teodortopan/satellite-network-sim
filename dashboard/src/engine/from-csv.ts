// The routing program's routes CSV, drops CSV and summary turned into the route story's
// directions, exactly as python/export_dashboard.py route_story writes story-route.json
// (dashboard/data-format.md "story-route.json"), in the parsed types of stories/route/data.ts.
// Values worked out here, as in the export: the steps each route interval spans, the changes
// (bad plus increases) and the total time without a route. Everything else is copied. Anything
// that is not what the program writes is refused with the file, the row and the reason.

import type { Direction, Drop, DropElement, RouteInterval, RouteSummary, StrategyRun } from '../stories/route/data';
import { utcSeconds } from './run';

/** The routes CSV columns the converter reads (apps/routing/run.hpp kRoutesColumns, in order). */
export const ROUTES_COLUMNS = [
  'utc',
  'from',
  'to',
  'role',
  'pattern',
  'model',
  'strategy',
  'status',
  'drop_cause',
  'one_way_ms',
  'ideal_ms',
  'laser_hops',
  'route',
  'first_norad_cat_id',
  'last_norad_cat_id',
  'serving_norad_cat_id',
  'gateway',
  'gateway_source',
  'home_pop',
  'plan',
  'backup_ms',
] as const;

/** The drops CSV columns (kDropsColumns). */
export const DROPS_COLUMNS = [
  'start_utc',
  'length_s',
  'from',
  'to',
  'role',
  'pattern',
  'model',
  'strategy',
  'cause',
  'satellite_norad_cat_id',
  'link_norad_cat_id_a',
  'link_norad_cat_id_b',
  'link_down_reason',
  'site',
  'site_kind',
] as const;

/** Catalogue numbers and names to the indices of the exported files; undefined if unknown. */
export interface Lookups {
  satellite: (norad: number) => number | undefined;
  city: (name: string) => number | undefined;
  gateway: (name: string) => number | undefined;
}

export interface EngineOutput {
  routesCsv: string;
  dropsCsv: string;
  summary: string;
}

export interface RunShape {
  /** The run's directions as (from, to) city names, in the routes CSV's order. */
  directions: readonly (readonly [string, string])[];
  strategies: readonly string[];
  start_utc: string;
  /** At a 1 s step, so a step is a second (the route story's rule). */
  duration_s: number;
}

export class EngineOutputError extends Error {
  constructor(
    file: string,
    where: string,
    problem: string,
  ) {
    super(`${file}${where === '' ? '' : `, ${where}`}: ${problem}`);
    this.name = 'EngineOutputError';
  }
}

type Row = Record<string, string>;

/** RFC 4180, as apps/common/csv.hpp writes it: one header, every row with every field. */
export function parseCsv(text: string, columns: readonly string[], file: string): Row[] {
  const records: string[][] = [];
  let field = '';
  let record: string[] = [];
  let quoted = false;
  let i = 0;
  const end = (): void => {
    record.push(field);
    records.push(record);
    field = '';
    record = [];
  };
  while (i < text.length) {
    const c = text[i] as string;
    if (quoted) {
      if (c === '"' && text[i + 1] === '"') {
        field += '"';
        i += 2;
        continue;
      }
      if (c === '"') {
        quoted = false;
        i += 1;
        if (i < text.length && text[i] !== ',' && text[i] !== '\n') {
          throw new EngineOutputError(file, `line ${records.length + 1}`, 'text after a closing quote');
        }
        continue;
      }
      field += c;
      i += 1;
      continue;
    }
    if (c === '"' && field === '') quoted = true;
    else if (c === ',') {
      record.push(field);
      field = '';
    } else if (c === '\n') end();
    else if (c === '\r') throw new EngineOutputError(file, `line ${records.length + 1}`, 'a carriage return');
    else field += c;
    i += 1;
  }
  if (quoted) throw new EngineOutputError(file, `line ${records.length + 1}`, 'a quote that does not close');
  if (field !== '' || record.length > 0) {
    throw new EngineOutputError(file, `line ${records.length + 1}`, 'the last line has no line end');
  }
  const header = records.shift();
  if (header === undefined) throw new EngineOutputError(file, '', 'is empty');
  if (header.join(',') !== columns.join(',')) {
    throw new EngineOutputError(file, 'header', `is "${header.join(',')}", expected "${columns.join(',')}"`);
  }
  return records.map((r, k) => {
    if (r.length !== columns.length) {
      throw new EngineOutputError(file, `row ${k + 1}`, `has ${r.length} fields, expected ${columns.length}`);
    }
    const row: Row = {};
    columns.forEach((name, j) => (row[name] = r[j] as string));
    return row;
  });
}

const DECIMAL = /^-?\d+(\.\d+)?(e[+-]?\d+)?$/;
const WHOLE = /^\d+$/;

function decimal(text: string, file: string, where: string, what: string): number {
  if (!DECIMAL.test(text)) throw new EngineOutputError(file, where, `${what} "${text}" is not a number`);
  return Number(text);
}

function whole(text: string, file: string, where: string, what: string): number {
  if (!WHOLE.test(text)) throw new EngineOutputError(file, where, `${what} "${text}" is not a whole number`);
  return Number(text);
}

// ---------------------------------------------------------------------------------------
// The summary, read as python/run_phase3.py parse_summary reads it.
// ---------------------------------------------------------------------------------------
const DIRECTION = /^ {4}(\S.*) -> (.*) \((\w+), (\d+) km, lower bound ([\d.]+) ms\)$/;
const STRATEGY = /^ {6}(\S+): ok (\d+) of (\d+) steps; plans (\d+) \(kept (\d+), replaced (\d+), none (\d+)\)$/;
const MEASURE = /^ {8}([a-z ]+): (.*)$/;
const COUNTS = /counts: bad (\d+), increases (\d+), breaks (\d+), recoveries (\d+)$/;
const QUANTILES =
  /^min (-?[\d.]+)[^,]*, p5 (-?[\d.]+)[^,]*, median (-?[\d.]+)[^,]*, p95 (-?[\d.]+)[^,]*, max (-?[\d.]+)[^(]*\((\d+)\)$/;

interface LaneSummary {
  ok?: number;
  steps?: number;
  bad?: number;
  increases?: number;
  breaks?: number;
  recoveries?: number;
  median?: number | null;
  drop_s?: Record<string, number>;
  drop_events?: number;
}

/** (from, to, strategy) joined by tabs -> what the summary says of that lane. */
export function readSummary(text: string): Map<string, LaneSummary> {
  const file = 'summary';
  const lanes = new Map<string, LaneSummary>();
  let direction: [string, string] | null = null;
  let lane: LaneSummary | null = null;
  text.split('\n').forEach((line, k) => {
    const where = `line ${k + 1}`;
    let m: RegExpExecArray | null;
    if ((m = DIRECTION.exec(line)) !== null) {
      direction = [m[1] as string, m[2] as string];
      lane = null;
    } else if (direction !== null && (m = STRATEGY.exec(line)) !== null) {
      lane = { ok: Number(m[2]), steps: Number(m[3]) };
      lanes.set([...direction, m[1] as string].join('\t'), lane);
    } else if (lane !== null && (m = MEASURE.exec(line)) !== null) {
      const what = m[1] as string;
      const value = m[2] as string;
      if (what === 'path changes per hour') {
        const c = COUNTS.exec(value);
        if (c === null) throw new EngineOutputError(file, where, 'no change counts');
        lane.bad = Number(c[1]);
        lane.increases = Number(c[2]);
        lane.breaks = Number(c[3]);
        lane.recoveries = Number(c[4]);
      } else if (what === 'no route') {
        const at = value.lastIndexOf('; drop events ');
        if (at < 0) throw new EngineOutputError(file, where, 'no drop events');
        lane.drop_events = whole(value.slice(at + '; drop events '.length), file, where, 'drop events');
        lane.drop_s = {};
        for (const [, cause, seconds] of value.slice(0, at).matchAll(/([a-z_]+) ([\d.]+) s/g)) {
          lane.drop_s[cause as string] = Number(seconds);
        }
      } else if (what === 'delay') {
        if (value.trim() === 'none') lane.median = null;
        else {
          const q = QUANTILES.exec(value.trim());
          if (q === null) throw new EngineOutputError(file, where, `cannot read the delay "${value}"`);
          lane.median = Number(q[3]);
        }
      }
    }
  });
  return lanes;
}

function summaryOf(lane: LaneSummary | undefined, where: string): RouteSummary {
  const need = <T>(value: T | undefined, what: string): T => {
    if (value === undefined) throw new EngineOutputError('summary', where, `has no ${what}`);
    return value;
  };
  const bad = need(lane?.bad, 'change counts');
  const increases = need(lane?.increases, 'change counts');
  const no_route_s = need(lane?.drop_s, 'no route line');
  let total = 0;
  for (const seconds of Object.values(no_route_s)) total += seconds;
  return {
    ok_steps: need(lane?.ok, 'strategy line'),
    steps: need(lane?.steps, 'strategy line'),
    delay_median_ms: need(lane?.median, 'delay line'),
    bad_changes: bad,
    increases,
    changes: bad + increases,
    breaks: need(lane?.breaks, 'change counts'),
    recoveries: need(lane?.recoveries, 'change counts'),
    drop_events: need(lane?.drop_events, 'no route line'),
    no_route_s,
    no_route_total_s: total,
  };
}

// ---------------------------------------------------------------------------------------
// The directions.
// ---------------------------------------------------------------------------------------
function lookup(found: number | undefined, file: string, where: string, what: string): number {
  if (found === undefined) throw new EngineOutputError(file, where, `${what} is not in the exported files`);
  return found;
}

function sameRoute(a: readonly number[], b: readonly number[]): boolean {
  return a.length === b.length && a.every((x, i) => x === b[i]);
}

function dropElement(r: Row, lookups: Lookups, where: string): DropElement | null {
  const file = 'drops CSV';
  const sat = (text: string): number =>
    lookup(lookups.satellite(whole(text, file, where, 'satellite')), file, where, `satellite ${text}`);
  if (r.satellite_norad_cat_id !== '') return { kind: 'satellite', sat: sat(r.satellite_norad_cat_id as string) };
  if (r.link_norad_cat_id_a !== '') {
    return {
      kind: 'link',
      a: sat(r.link_norad_cat_id_a as string),
      b: sat(r.link_norad_cat_id_b as string),
      reason: r.link_down_reason as string,
    };
  }
  if (r.site !== '') {
    const name = r.site as string;
    if (r.site_kind === 'city') return { kind: 'city', site: lookup(lookups.city(name), file, where, `city ${name}`) };
    if (r.site_kind === 'gateway') {
      return { kind: 'gateway', site: lookup(lookups.gateway(name), file, where, `gateway ${name}`) };
    }
    throw new EngineOutputError(file, where, `site kind "${r.site_kind ?? ''}" is not city or gateway`);
  }
  return null;
}

/** The route story's directions of one run (story-route.json "directions"). */
export function directionsFromCsv(output: EngineOutput, shape: RunShape, lookups: Lookups): Direction[] {
  const start = utcSeconds(shape.start_utc);
  if (start === null) throw new EngineOutputError('run', '', `start ${shape.start_utc} is not a time`);
  const steps = shape.duration_s + 1;
  const routes = parseCsv(output.routesCsv, ROUTES_COLUMNS, 'routes CSV');
  const drops = parseCsv(output.dropsCsv, DROPS_COLUMNS, 'drops CSV');
  const summary = readSummary(output.summary);
  const key = (from: string, to: string, strategy: string): string => [from, to, strategy].join('\t');
  const wanted = new Set(shape.directions.flatMap(([a, b]) => shape.strategies.map((s) => key(a, b, s))));

  const lanes = new Map<string, Row[]>();
  routes.forEach((r, k) => {
    const name = key(r.from as string, r.to as string, r.strategy as string);
    if (!wanted.has(name)) {
      throw new EngineOutputError('routes CSV', `row ${k + 1}`, `${name.replaceAll('\t', ' ')} is not a direction and strategy of the run`);
    }
    const list = lanes.get(name) ?? [];
    list.push(r);
    lanes.set(name, list);
  });
  const laneDrops = new Map<string, Row[]>();
  drops.forEach((r, k) => {
    const name = key(r.from as string, r.to as string, r.strategy as string);
    if (!wanted.has(name)) {
      throw new EngineOutputError('drops CSV', `row ${k + 1}`, `${name.replaceAll('\t', ' ')} is not a direction and strategy of the run`);
    }
    const list = laneDrops.get(name) ?? [];
    list.push(r);
    laneDrops.set(name, list);
  });
  const secondsAfter = (text: string, file: string, where: string): number => {
    const t = utcSeconds(text);
    if (t === null) throw new EngineOutputError(file, where, `time "${text}" is not YYYY-MM-DDTHH:MM:SS`);
    return t - start;
  };

  return shape.directions.map(([from, to]) => {
    const strategies: Record<string, StrategyRun> = {};
    for (const s of shape.strategies) {
      const where = `${from} -> ${to} ${s}`;
      const rows = lanes.get(key(from, to, s)) ?? [];
      if (rows.length !== steps) {
        throw new EngineOutputError('routes CSV', where, `${rows.length} rows, expected ${steps}`);
      }
      const delay_ms: (number | null)[] = [];
      const intervals: RouteInterval[] = [];
      rows.forEach((r, k) => {
        const at = `${where} step ${k}`;
        if (secondsAfter(r.utc as string, 'routes CSV', at) !== k) {
          throw new EngineOutputError('routes CSV', at, `is at ${r.utc ?? ''}, not ${k} s after the start`);
        }
        if (r.status === 'drop') {
          delay_ms.push(null);
          return;
        }
        if (r.status !== 'ok') throw new EngineOutputError('routes CSV', at, `status "${r.status ?? ''}" is not ok or drop`);
        delay_ms.push(decimal(r.one_way_ms as string, 'routes CSV', at, 'one_way_ms'));
        const norads = (r.route as string).split(' ');
        const route = norads.map((n) =>
          lookup(lookups.satellite(whole(n, 'routes CSV', at, 'satellite')), 'routes CSV', at, `satellite ${n}`),
        );
        const g = r.gateway as string;
        const gateway = g === '' ? null : lookup(lookups.gateway(g), 'routes CSV', at, `gateway ${g}`);
        const last = intervals[intervals.length - 1];
        if (last !== undefined && last.last_s === k - 1 && sameRoute(last.route, route) && last.gateway === gateway) {
          last.last_s = k;
        } else {
          intervals.push({ first_s: k, last_s: k, route, gateway });
        }
      });
      const laneDropList: Drop[] = (laneDrops.get(key(from, to, s)) ?? []).map((r, k) => {
        const at = `${where} drop ${k}`;
        return {
          start_s: secondsAfter(r.start_utc as string, 'drops CSV', at),
          length_s: decimal(r.length_s as string, 'drops CSV', at, 'length_s'),
          cause: r.cause as string,
          element: dropElement(r, lookups, at),
        };
      });
      strategies[s] = {
        intervals,
        delay_ms,
        drops: laneDropList,
        summary: summaryOf(summary.get(key(from, to, s)), where),
      };
    }
    return {
      from: lookup(lookups.city(from), 'routes CSV', '', `city ${from}`),
      to: lookup(lookups.city(to), 'routes CSV', '', `city ${to}`),
      strategies,
    };
  });
}

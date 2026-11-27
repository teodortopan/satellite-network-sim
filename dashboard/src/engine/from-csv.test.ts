import { describe, expect, it } from 'vitest';
import { DROPS_COLUMNS, directionsFromCsv, EngineOutputError, parseCsv, ROUTES_COLUMNS, type EngineOutput, type Lookups, type RunShape } from './from-csv';

// A hand-made run: A to B and back, one strategy, 6 s (7 steps). Forward: route 101 102 over
// steps 0-1, a change to 101 103 at 2-3 (through gateway G), a drop at 4-5, 101 103 again at 6.
// Backward: always 201, one link drop is not in its rows (drops are per lane).

const SATS = new Map([
  [101, 0],
  [102, 1],
  [103, 2],
  [201, 3],
]);
const CITIES = new Map([
  ['A', 0],
  ['B', 1],
]);
const GATEWAYS = new Map([['G', 2]]);
const LOOKUPS: Lookups = {
  satellite: (n) => SATS.get(n),
  city: (n) => CITIES.get(n),
  gateway: (n) => GATEWAYS.get(n),
};
const SHAPE: RunShape = {
  directions: [
    ['A', 'B'],
    ['B', 'A'],
  ],
  strategies: ['lowest-delay'],
  start_utc: '2026-11-18T21:30:30',
  duration_s: 6,
};

function utc(k: number): string {
  return `2026-11-18T21:30:${String(30 + k).padStart(2, '0')}`;
}

function routeRow(k: number, from: string, to: string, ok: boolean, route = '', gateway = '', ms = ''): string {
  const fields: Record<string, string> = {
    utc: utc(k),
    from,
    to,
    role: 'study',
    pattern: 'three',
    model: 'best-case',
    strategy: 'lowest-delay',
    status: ok ? 'ok' : 'drop',
    drop_cause: ok ? '' : 'ground_link_lost',
    one_way_ms: ms,
    ideal_ms: '20',
    route,
    gateway,
  };
  return ROUTES_COLUMNS.map((c) => fields[c] ?? '').join(',');
}

function dropRow(k: number, length: number, from: string, to: string, element: Record<string, string>): string {
  const fields: Record<string, string> = {
    start_utc: utc(k),
    length_s: String(length),
    from,
    to,
    role: 'study',
    pattern: 'three',
    model: 'best-case',
    strategy: 'lowest-delay',
    cause: 'ground_link_lost',
    ...element,
  };
  return DROPS_COLUMNS.map((c) => fields[c] ?? '').join(',');
}

function lane(name: string, ok: number, bad: number, increases: number, noRoute: string, events: number, delay: string): string {
  return [
    `      ${name}: ok ${ok} of 7 steps; plans 1 (kept 0, replaced 1, none 0)`,
    `        path changes per hour: 1.0 (bad 1.0, delay increases 0.0), breaks 1.0; counts: bad ${bad}, increases ${increases}, breaks 1, recoveries 1`,
    '        change size: none',
    `        delay: ${delay}`,
    `        no route:${noRoute}; drop events ${events}`,
  ].join('\n');
}

function output(): EngineOutput {
  const forward = [
    routeRow(0, 'A', 'B', true, '101 102', '', '20.5'),
    routeRow(1, 'A', 'B', true, '101 102', '', '20.25'),
    routeRow(2, 'A', 'B', true, '101 103', 'G', '21'),
    routeRow(3, 'A', 'B', true, '101 103', 'G', '21.125'),
    routeRow(4, 'A', 'B', false),
    routeRow(5, 'A', 'B', false),
    routeRow(6, 'A', 'B', true, '101 103', 'G', '21.5'),
  ];
  const backward = Array.from({ length: 7 }, (_, k) => routeRow(k, 'B', 'A', true, '201', '', '30'));
  const rows = forward.flatMap((f, k) => [f, backward[k] as string]);
  const drops = [
    dropRow(4, 2, 'A', 'B', { satellite_norad_cat_id: '102', site: 'A', site_kind: 'city' }),
  ];
  const summary = [
    'routing summary',
    '  routes per pair, direction and strategy (changes per hour over 0.002 h):',
    '    A -> B (study, 5570 km, lower bound 18.580 ms)',
    '      ideal: min 20.000 ms, p5 20.000 ms, median 20.000 ms, p95 20.000 ms, max 20.000 ms (7)',
    lane('lowest-delay', 5, 1, 2, ' ground_link_lost 2.0 s no_path 0.5 s', 1, 'min 20.250 ms, p5 20.300 ms, median 21.000 ms, p95 21.400 ms, max 21.500 ms (5)'),
    '    B -> A (study, 5570 km, lower bound 18.580 ms)',
    lane('lowest-delay', 7, 0, 0, ' none', 0, 'min 30.000 ms, p5 30.000 ms, median 30.000 ms, p95 30.000 ms, max 30.000 ms (7)'),
    '  checks: PASSED',
    '',
  ].join('\n');
  return {
    routesCsv: `${ROUTES_COLUMNS.join(',')}\n${rows.join('\n')}\n`,
    dropsCsv: `${DROPS_COLUMNS.join(',')}\n${drops.join('\n')}\n`,
    summary,
  };
}

function problem(out: EngineOutput, shape: RunShape = SHAPE, lookups: Lookups = LOOKUPS): string {
  try {
    directionsFromCsv(out, shape, lookups);
  } catch (error) {
    if (error instanceof EngineOutputError) return error.message;
    throw error;
  }
  return 'accepted';
}

describe('the engine output as route story directions', () => {
  it('turn an ok run into intervals, delays, drops and a summary', () => {
    const [ab, ba] = directionsFromCsv(output(), SHAPE, LOOKUPS);
    expect(ab?.from).toBe(0);
    expect(ab?.to).toBe(1);
    const run = ab?.strategies['lowest-delay'];
    expect(run?.intervals).toEqual([
      { first_s: 0, last_s: 1, route: [0, 1], gateway: null },
      { first_s: 2, last_s: 3, route: [0, 2], gateway: 2 },
      { first_s: 6, last_s: 6, route: [0, 2], gateway: 2 },
    ]);
    expect(run?.delay_ms).toEqual([20.5, 20.25, 21, 21.125, null, null, 21.5]);
    expect(run?.drops).toEqual([{ start_s: 4, length_s: 2, cause: 'ground_link_lost', element: { kind: 'satellite', sat: 1 } }]);
    expect(run?.summary).toEqual({
      ok_steps: 5,
      steps: 7,
      delay_median_ms: 21,
      bad_changes: 1,
      increases: 2,
      changes: 3,
      breaks: 1,
      recoveries: 1,
      drop_events: 1,
      no_route_s: { ground_link_lost: 2, no_path: 0.5 },
      no_route_total_s: 2.5,
    });
    const back = ba?.strategies['lowest-delay'];
    expect(back?.intervals).toEqual([{ first_s: 0, last_s: 6, route: [3], gateway: null }]);
    expect(back?.drops).toEqual([]);
    expect(back?.summary.changes).toBe(0);
    expect(back?.summary.no_route_total_s).toBe(0);
    expect(back?.summary.no_route_s).toEqual({});
  });

  it('read every kind of drop element', () => {
    const out = output();
    const element = (e: Record<string, string>): unknown => {
      out.dropsCsv = `${DROPS_COLUMNS.join(',')}\n${dropRow(4, 2, 'A', 'B', e)}\n`;
      return directionsFromCsv(out, SHAPE, LOOKUPS)[0]?.strategies['lowest-delay']?.drops[0]?.element;
    };
    expect(element({ satellite_norad_cat_id: '103' })).toEqual({ kind: 'satellite', sat: 2 });
    expect(element({ link_norad_cat_id_a: '101', link_norad_cat_id_b: '103', link_down_reason: 'no_line_of_sight' })).toEqual({
      kind: 'link',
      a: 0,
      b: 2,
      reason: 'no_line_of_sight',
    });
    expect(element({ site: 'B', site_kind: 'city' })).toEqual({ kind: 'city', site: 1 });
    expect(element({ site: 'G', site_kind: 'gateway' })).toEqual({ kind: 'gateway', site: 2 });
    expect(element({})).toBeNull();
  });

  it('keep a route change at the step it happens, and join only consecutive steps', () => {
    const out = output();
    // Step 3 back to 101 102: three intervals before the drop.
    out.routesCsv = out.routesCsv.replace(routeRow(3, 'A', 'B', true, '101 103', 'G', '21.125'), routeRow(3, 'A', 'B', true, '101 102', '', '21.125'));
    const run = directionsFromCsv(out, SHAPE, LOOKUPS)[0]?.strategies['lowest-delay'];
    expect(run?.intervals.map((i) => [i.first_s, i.last_s])).toEqual([
      [0, 1],
      [2, 2],
      [3, 3],
      [6, 6],
    ]);
  });

  it('start a new interval after a drop of one step, even on the same route', () => {
    const out = output();
    // Step 5 back on 101 103: the drop is step 4 alone, and 101 103 spans 2-3, then 5-6.
    out.routesCsv = out.routesCsv.replace(routeRow(5, 'A', 'B', false), routeRow(5, 'A', 'B', true, '101 103', 'G', '21.25'));
    const run = directionsFromCsv(out, SHAPE, LOOKUPS)[0]?.strategies['lowest-delay'];
    expect(run?.intervals.map((i) => [i.first_s, i.last_s])).toEqual([
      [0, 1],
      [2, 3],
      [5, 6],
    ]);
  });

  it('read CSV fields in quotes', () => {
    const rows = parseCsv('a,b\n"x, ""y""",2\n', ['a', 'b'], 'test');
    expect(rows).toEqual([{ a: 'x, "y"', b: '2' }]);
  });

  it('refuse malformed output', () => {
    const header = output();
    header.routesCsv = header.routesCsv.replace('one_way_ms', 'delay_ms');
    expect(problem(header)).toMatch(/^routes CSV, header: is /);

    const short = output();
    short.routesCsv = short.routesCsv.split('\n').slice(0, -2).join('\n') + '\n';
    expect(problem(short)).toBe('routes CSV, B -> A lowest-delay: 6 rows, expected 7');

    const status = output();
    status.routesCsv = status.routesCsv.replace(',drop,ground_link_lost,', ',lost,ground_link_lost,');
    expect(problem(status)).toBe('routes CSV, A -> B lowest-delay step 4: status "lost" is not ok or drop');

    const unknown = output();
    unknown.routesCsv = unknown.routesCsv.replace('101 102', '101 999');
    expect(problem(unknown)).toBe('routes CSV, A -> B lowest-delay step 0: satellite 999 is not in the exported files');

    const fields = output();
    fields.dropsCsv = fields.dropsCsv.replace(',city\n', ',city,extra\n');
    expect(problem(fields)).toBe('drops CSV, row 1: has 16 fields, expected 15');

    const kind = output();
    kind.dropsCsv = `${DROPS_COLUMNS.join(',')}\n${dropRow(4, 2, 'A', 'B', { site: 'A', site_kind: 'pop' })}\n`;
    expect(problem(kind)).toMatch(/site kind "pop" is not city or gateway/);

    const time = output();
    time.routesCsv = time.routesCsv.replace(`${utc(2)},A,B`, `${utc(3)},A,B`);
    expect(problem(time)).toMatch(/A -> B lowest-delay step 2: is at 2026-11-18T21:30:33, not 2 s after the start/);

    const summary = output();
    summary.summary = summary.summary.replace('    B -> A (study', '    B -> C (study');
    expect(problem(summary)).toBe('summary, B -> A lowest-delay: has no change counts');

    const end = output();
    end.dropsCsv = end.dropsCsv.trimEnd();
    expect(problem(end)).toBe('drops CSV, line 2: the last line has no line end');

    expect(problem({ routesCsv: '', dropsCsv: '', summary: '' })).toBe('routes CSV: is empty');
    expect(problem(output(), { ...SHAPE, strategies: ['keep-until-broken'] })).toMatch(/is not a direction and strategy of the run/);
  });
});

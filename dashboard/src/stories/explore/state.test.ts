import { describe, expect, it } from 'vitest';
import type { Place } from '../../data/places';
import { DROPS_COLUMNS, directionsFromCsv, ROUTES_COLUMNS, type Lookups } from '../../engine/from-csv';
import type { EngineManifest } from '../../engine/settings';
import type { Direction } from '../route/data';
import { decodeTrip, encodeTrip, headlineAt, nearestPlace, pairProblem, pairProblemText, pointName, runWindow, windowWords } from './state';

// ---------------------------------------------------------------------------------------
// A hand-made engine output: Lagos and Sydney, both directions, lowest-delay only, 4 s (5
// steps). Lagos to Sydney: 101 102 103 at steps 0-1, no connection at 2 (the satellite over
// Lagos left), 101 104 at 3-4. The other direction: 201 throughout.
// ---------------------------------------------------------------------------------------

const START = '2026-11-18T21:30:30';
const utc = (k: number): string => `2026-11-18T21:30:${String(30 + k).padStart(2, '0')}`;
const SATS = new Map([101, 102, 103, 104, 201].map((n, i) => [n, i]));
const LOOKUPS: Lookups = {
  satellite: (n) => SATS.get(n),
  city: (name) => (name === 'Lagos' ? 10 : name === 'Sydney' ? 11 : undefined),
  gateway: () => undefined,
};

function routeRow(k: number, from: string, to: string, route: string | null, ms: string, cause = ''): string {
  const fields: Record<string, string> = {
    utc: utc(k),
    from,
    to,
    role: 'study',
    pattern: 'three',
    model: 'best-case',
    strategy: 'lowest-delay',
    status: route === null ? 'drop' : 'ok',
    drop_cause: cause,
    one_way_ms: ms,
    route: route ?? '',
  };
  return ROUTES_COLUMNS.map((c) => fields[c] ?? '').join(',');
}

function dropRow(k: number, length: number, from: string, to: string, cause: string, element: Record<string, string>): string {
  const fields: Record<string, string> = {
    start_utc: utc(k),
    length_s: String(length),
    from,
    to,
    role: 'study',
    pattern: 'three',
    model: 'best-case',
    strategy: 'lowest-delay',
    cause,
    ...element,
  };
  return DROPS_COLUMNS.map((c) => fields[c] ?? '').join(',');
}

function lane(ok: number, noRoute: string, events: number, delay: string): string {
  return [
    `      lowest-delay: ok ${ok} of 5 steps; plans 1 (kept 0, replaced 1, none 0)`,
    '        path changes per hour: 0.0 (bad 0.0, delay increases 0.0), breaks 0.0; counts: bad 0, increases 0, breaks 1, recoveries 1',
    `        delay: ${delay}`,
    `        no route:${noRoute}; drop events ${events}`,
  ].join('\n');
}

function summary(forward: string, backward: string): string {
  return ['routing summary', '    Lagos -> Sydney (study, 15000 km, lower bound 50 ms)', forward, '    Sydney -> Lagos (study, 15000 km, lower bound 50 ms)', backward, ''].join('\n');
}

function directions(routes: string[], drops: string[], text: string): Direction[] {
  return directionsFromCsv(
    {
      routesCsv: `${ROUTES_COLUMNS.join(',')}\n${routes.join('\n')}\n`,
      dropsCsv: `${DROPS_COLUMNS.join(',')}\n${drops.map((d) => `${d}\n`).join('')}`,
      summary: text,
    },
    {
      directions: [
        ['Lagos', 'Sydney'],
        ['Sydney', 'Lagos'],
      ],
      strategies: ['lowest-delay'],
      start_utc: START,
      duration_s: 4,
    },
    LOOKUPS,
  );
}

const DELAY = 'min 80.000 ms, p5 80.000 ms, median 81.000 ms, p95 82.000 ms, max 82.000 ms (4)';

function connected(): Direction[] {
  const forward = [
    routeRow(0, 'Lagos', 'Sydney', '101 102 103', '80.25'),
    routeRow(1, 'Lagos', 'Sydney', '101 102 103', '80.5'),
    routeRow(2, 'Lagos', 'Sydney', null, '', 'ground_link_lost'),
    routeRow(3, 'Lagos', 'Sydney', '101 104', '82.125'),
    routeRow(4, 'Lagos', 'Sydney', '101 104', '81'),
  ];
  const backward = [0, 1, 2, 3, 4].map((k) => routeRow(k, 'Sydney', 'Lagos', '201', '90'));
  return directions(
    forward.flatMap((f, k) => [f, backward[k] as string]),
    [dropRow(2, 1, 'Lagos', 'Sydney', 'ground_link_lost', { satellite_norad_cat_id: '101', site: 'Lagos', site_kind: 'city' })],
    summary(lane(4, ' ground_link_lost 1.0 s', 1, DELAY), lane(5, ' none', 0, 'min 90.000 ms, p5 90.000 ms, median 90.000 ms, p95 90.000 ms, max 90.000 ms (5)')),
  );
}

const NAMES = { from: 'Lagos', to: 'Sydney' };

describe('the headline, from the engine output', () => {
  it('gives the fastest rule’s delay and satellites at the clock’s time', () => {
    const [forward] = connected();
    const h0 = headlineAt(forward as Direction, NAMES, 0);
    expect(h0).toEqual({ kind: 'route', text: 'Lagos → Sydney: about 80.3 ms one way, through 3 satellites', delay_ms: 80.25, satellites: 3 });
    const h1 = headlineAt(forward as Direction, NAMES, 1.9);
    expect(h1.kind === 'route' && h1.delay_ms).toBe(80.5);
    const h3 = headlineAt(forward as Direction, NAMES, 3);
    expect(h3).toEqual({ kind: 'route', text: 'Lagos → Sydney: about 82.1 ms one way, through 2 satellites', delay_ms: 82.125, satellites: 2 });
    expect(headlineAt(forward as Direction, NAMES, 4).text).toBe('Lagos → Sydney: about 81 ms one way, through 2 satellites');
  });

  it('says there is no connection at a moment without one, with the engine’s cause', () => {
    const [forward] = connected();
    expect(headlineAt(forward as Direction, NAMES, 2.5)).toEqual({
      kind: 'drop',
      text: 'Lagos → Sydney: no connection at this moment, because the link between a place and its satellite was lost',
      cause: 'ground_link_lost',
    });
  });

  it('follows the other direction with the names given', () => {
    const [, backward] = connected();
    expect(headlineAt(backward as Direction, { from: 'Sydney', to: 'Lagos' }, 2).text).toBe('Sydney → Lagos: about 90 ms one way, through 1 satellite');
  });

  it('says plainly when no satellite comes within view of a place for the whole run', () => {
    const rows = [0, 1, 2, 3, 4].flatMap((k) => [routeRow(k, 'Lagos', 'Sydney', null, '', 'no_coverage'), routeRow(k, 'Sydney', 'Lagos', null, '', 'no_coverage')]);
    const drops = [
      dropRow(0, 5, 'Lagos', 'Sydney', 'no_coverage', { site: 'Sydney', site_kind: 'city' }),
      dropRow(0, 5, 'Sydney', 'Lagos', 'no_coverage', { site: 'Sydney', site_kind: 'city' }),
    ];
    const none = lane(0, ' no_coverage 5.0 s', 1, 'none');
    const [forward, backward] = directions(rows, drops, summary(none, none));
    for (const t of [0, 2, 4]) {
      expect(headlineAt(forward as Direction, NAMES, t)).toEqual({
        kind: 'never',
        text: 'No Starlink satellite of this snapshot passes within view of Sydney during these minutes, so no message can get through.',
        cause: 'no_coverage',
      });
    }
    expect(headlineAt(backward as Direction, { from: 'Sydney', to: 'Lagos' }, 0).text).toContain('within view of Sydney');
  });

  it('names another cause of a run without any connection in plain words', () => {
    const rows = [0, 1, 2, 3, 4].flatMap((k) => [routeRow(k, 'Lagos', 'Sydney', null, '', 'no_path'), routeRow(k, 'Sydney', 'Lagos', '201', '90')]);
    const drops = [dropRow(0, 5, 'Lagos', 'Sydney', 'no_path', {})];
    const [forward] = directions(rows, drops, summary(lane(0, ' no_path 5.0 s', 1, 'none'), lane(5, ' none', 0, DELAY)));
    expect(headlineAt(forward as Direction, NAMES, 1)).toEqual({
      kind: 'never',
      text: 'No connection between Lagos and Sydney at any moment of these minutes: no chain of satellites joins the two places.',
      cause: 'no_path',
    });
  });
});

describe('the address keeps the choice', () => {
  const trip = { from: { name: 'São Paulo', lat_deg: -23.5567, lon_deg: -46.627 }, to: { name: 'Point at 52.1° N, 4.3° E', lat_deg: 52.123456, lon_deg: 4.3 } };

  it('writes names and coordinates, and reads them back', () => {
    const hash = encodeTrip('explore', trip);
    expect(hash).toBe('#explore?from=S%C3%A3o%20Paulo,-23.5567,-46.627&to=Point%20at%2052.1%C2%B0%20N%2C%204.3%C2%B0%20E,52.1235,4.3');
    expect(decodeTrip(hash)).toEqual({ from: trip.from, to: { ...trip.to, lat_deg: 52.1235 } });
  });

  it('holds nothing without a choice, and ignores a broken one', () => {
    expect(decodeTrip('#explore')).toBeNull();
    expect(decodeTrip('#explore?from=A,1,2')).toBeNull();
    expect(decodeTrip('#explore?from=A,91,2&to=B,0,0')).toBeNull();
    expect(decodeTrip('#explore?from=A,1,2,3&to=B,0,0')).toBeNull();
    expect(decodeTrip('#explore?from=,1,2&to=B,0,0')).toBeNull();
    expect(decodeTrip('#explore?from=A,1e2,2&to=B,0,0')).toBeNull();
    expect(decodeTrip('#explore?from=%E0%A4%A,1,2&to=B,0,0')).toBeNull();
    expect(decodeTrip('#explore?to=B,-1.5,-2&from=A,1,2')).toEqual({ from: { name: 'A', lat_deg: 1, lon_deg: 2 }, to: { name: 'B', lat_deg: -1.5, lon_deg: -2 } });
  });
});

describe('places picked on the globe', () => {
  const place = (name: string, lat: number, lon: number, population: number): Place => ({ name, region: '', country: 'C', lat_deg: lat, lon_deg: lon, population });
  const places = [place('Leiden', 52.16, 4.49, 120_000), place('The Hague', 52.08, 4.27, 500_000), place('Faraway', 10, 10, 1)];

  it('are named by their coordinates', () => {
    expect(pointName(52.12, 4.31)).toBe('Point at 52.1° N, 4.3° E');
    expect(pointName(-33.87, -151.21)).toBe('Point at 33.9° S, 151.2° W');
    expect(pointName(0.01, 180)).toBe('Point at 0.0°, 180.0°');
  });

  it('take the name of the nearest listed place within 30 km', () => {
    expect(nearestPlace(places, { lat_deg: 52.1, lon_deg: 4.3 })?.name).toBe('The Hague');
    expect(nearestPlace(places, { lat_deg: 52.16, lon_deg: 4.48 })?.name).toBe('Leiden');
    expect(nearestPlace(places, { lat_deg: 53, lon_deg: 4.3 })).toBeNull();
  });
});

describe('pairs that make no sense', () => {
  const at = (name: string, lat: number, lon: number): { name: string; lat_deg: number; lon_deg: number } => ({ name, lat_deg: lat, lon_deg: lon });

  it('are the same place twice, or two places a few km apart', () => {
    expect(pairProblem({ from: at('Lagos', 6.45, 3.39), to: at('Lagos', 6.45, 3.39) })).toEqual({ kind: 'same' });
    const close = { from: at('Leiden', 52.16, 4.49), to: at('The Hague', 52.08, 4.27) };
    const problem = pairProblem(close);
    expect(problem?.kind).toBe('close');
    expect(pairProblemText(close, problem ?? { kind: 'same' })).toMatch(/^Leiden and The Hague are only 17 km apart/);
    expect(pairProblem({ from: at('Lagos', 6.45, 3.39), to: at('Sydney', -33.87, 151.21) })).toBeNull();
    expect(pairProblemText({ from: at('Lagos', 6, 3), to: at('Lagos', 6, 3) }, { kind: 'same' })).toBe('From and To are the same place (Lagos). Choose two different places.');
  });
});

describe('the run of the page', () => {
  const engine = (duration_s: number): EngineManifest =>
    ({ window: { start_utc: START, duration_s }, settings: { step_s: 1 } }) as unknown as EngineManifest;

  it('starts at the window’s start and lasts at most 10 minutes', () => {
    expect(runWindow(engine(420))).toEqual({ start_utc: START, duration_s: 420 });
    expect(runWindow(engine(7200))).toEqual({ start_utc: START, duration_s: 600 });
  });

  it('is described by its day and minutes', () => {
    expect(windowWords(START, 420)).toBe('7 minutes of 18 November 2026, from 21:30:30 to 21:37:30 UTC (world time)');
  });
});

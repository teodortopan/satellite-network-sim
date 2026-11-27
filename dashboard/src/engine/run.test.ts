import { describe, expect, it } from 'vitest';
import { citiesFile, EngineRequestError, pointNames, runEngine, sanitizeName, stepsOf, type EngineFactory, type EngineModule, type EngineModuleOptions, type EngineRequest } from './run';
import type { EngineManifest } from './settings';

const ENGINE: EngineManifest = {
  program: { js: 'engine/satsim-routing.js', wasm: 'engine/satsim-routing.wasm' },
  snapshot: ['engine/snapshot/gp.json', 'engine/snapshot/finals2000A.all'],
  ground: ['engine/ground/gateways.json'],
  settings: { model: 'best-case', pattern: 'three', replan_s: 15, planner: 'start', backup: 'on', step_s: 1, plan_time_utc: '2026-11-18T21:15:00', roles: 'study' },
  strategies: ['lowest-delay', 'keep-until-broken', 'threshold:2ms'],
  window: { start_utc: '2026-11-18T21:30:30', duration_s: 420 },
  arguments: ['--snapshot', '{snapshot}', '--ground', '{ground}', '--cities', '{cities}', '--start', '{start_utc}', '--duration-s', '{duration_s}', '--strategies', '{strategies}', '--out', '-', '--drops-out', '{drops}', '--plan-time', '2026-11-18T21:15:00'],
};

const FILES = new Map([
  ['engine/snapshot/gp.json', new Uint8Array([1, 2])],
  ['engine/snapshot/finals2000A.all', new Uint8Array([3])],
  ['engine/ground/gateways.json', new Uint8Array([4, 5, 6])],
]);

interface Fake {
  factory: EngineFactory;
  calls: string[][];
  files: Map<string, Uint8Array | string>;
  instances: number;
}

/** A stand-in for the Emscripten module: an in-memory FS and a main that writes one row per direction and strategy per step. */
function fake(exitCode = 0): Fake {
  const state: Fake = { factory: async () => Promise.reject(new Error('unused')), calls: [], files: new Map(), instances: 0 };
  state.factory = (options: EngineModuleOptions): Promise<EngineModule> => {
    state.instances += 1;
    const files = new Map<string, Uint8Array | string>();
    state.files = files;
    const module: EngineModule = {
      FS: {
        mkdirTree: () => undefined,
        writeFile: (path, data) => files.set(path, data),
        readFile: (path) => {
          const data = files.get(path);
          if (data === undefined) throw new Error(`no ${path}`);
          return typeof data === 'string' ? data : new TextDecoder().decode(data);
        },
        analyzePath: (path) => ({ exists: files.has(path) }),
      },
      callMain: (args) => {
        state.calls.push(args);
        if (exitCode === 2) {
          files.set('/run/messages.txt', 'routing: --start is not a time\n');
          files.set('/run/summary.txt', '');
          return 2;
        }
        const value = (flag: string): string => args[args.indexOf(flag) + 1] ?? '';
        const strategies = value('--strategies').split(',');
        const steps = Number(value('--duration-s')) + 1;
        let csv = 'header\n';
        let lines = 1;
        options.onRoutesLine(lines);
        for (let k = 0; k < steps; k++) {
          for (let row = 0; row < 2 * strategies.length; row++) {
            csv += `row ${k}\n`;
            options.onRoutesLine(++lines);
          }
        }
        files.set('/run/routes.csv', csv);
        files.set(value('--drops-out'), 'drops header\n');
        files.set('/run/summary.txt', `routing summary\n  checks: ${exitCode === 0 ? 'PASSED' : 'FAILED'}\n`);
        files.set('/run/messages.txt', exitCode === 0 ? '' : 'routing: checks failed\n');
        return exitCode;
      },
      HEAPU8: new Uint8Array(64),
    };
    return Promise.resolve(module);
  };
  return state;
}

function request(f: Fake, extra: Partial<EngineRequest> = {}): EngineRequest {
  return {
    factory: f.factory,
    wasm: new Uint8Array([0]),
    files: FILES,
    engine: ENGINE,
    from: { name: 'Reykjavík, Iceland', lat_deg: 64.1, lon_deg: -21.9 },
    to: { name: 'Vladivostok', lat_deg: 43.1, lon_deg: 131.9 },
    start_utc: '2026-11-18T21:31:00',
    duration_s: 4,
    ...extra,
  };
}

describe('a run of the engine', () => {
  it('write the inputs, fill in the arguments and report progress', async () => {
    const f = fake();
    const progress: [number, number][] = [];
    const result = await runEngine(request(f, { onProgress: (done, total) => progress.push([done, total]) }));
    expect(result.exitCode).toBe(0);
    expect(f.calls).toEqual([
      ['--snapshot', '/data/snapshot', '--ground', '/data/ground', '--cities', '/data/cities.json', '--start', '2026-11-18T21:31:00', '--duration-s', '4', '--strategies', 'lowest-delay,keep-until-broken,threshold:2ms', '--out', '-', '--drops-out', '/run/drops.csv', '--plan-time', '2026-11-18T21:15:00'],
    ]);
    expect(f.files.get('/data/snapshot/gp.json')).toEqual(new Uint8Array([1, 2]));
    expect(f.files.get('/data/snapshot/finals2000A.all')).toEqual(new Uint8Array([3]));
    expect(f.files.get('/data/ground/gateways.json')).toEqual(new Uint8Array([4, 5, 6]));
    expect(f.files.get('/data/cities.json')).toBe(result.citiesJson);
    expect(JSON.parse(result.citiesJson)).toEqual({
      note: "One pair for the dashboard's on-demand route: Reykjavík Iceland to Vladivostok.",
      cities: [
        { name: 'Reykjavík Iceland', country: 'ZZ', region: 'ZZ', geofeed_city: 'Reykjavík Iceland', lat: 64.1, lon: -21.9 },
        { name: 'Vladivostok', country: 'ZZ', region: 'ZZ', geofeed_city: 'Vladivostok', lat: 43.1, lon: 131.9 },
      ],
      pairs: [{ from: 'Reykjavík Iceland', to: 'Vladivostok', role: 'study', why: 'chosen on the dashboard' }],
    });
    expect(progress).toEqual([
      [0, 5],
      [1, 5],
      [2, 5],
      [3, 5],
      [4, 5],
      [5, 5],
    ]);
    expect(result.routesCsv.split('\n')).toHaveLength(1 + 5 * 6 + 1);
    expect(result.dropsCsv).toBe('drops header\n');
    expect(result.summary).toBe('routing summary\n  checks: PASSED\n');
    expect(result.stderr).toBe('');
    expect(result.names).toEqual({ from: 'Reykjavík Iceland', to: 'Vladivostok' });
    expect(result.memoryBytes).toBe(64);
  });

  it('use a fresh module for every run and the strategies asked for', async () => {
    const f = fake();
    await runEngine(request(f));
    await runEngine(request(f, { strategies: ['longest-lived'] }));
    expect(f.instances).toBe(2);
    expect(f.calls[1]?.[f.calls[1].indexOf('--strategies') + 1]).toBe('longest-lived');
  });

  it('keep the country and region of a saved city', () => {
    const json = JSON.parse(citiesFile({ name: 'London', lat_deg: 51.5, lon_deg: -0.1, country: 'GB', region: 'GB-LND', geofeed_city: 'London' }, { name: 'X', lat_deg: 0, lon_deg: 0 })) as { cities: unknown[] };
    expect(json.cities[0]).toEqual({ name: 'London', country: 'GB', region: 'GB-LND', geofeed_city: 'London', lat: 51.5, lon: -0.1 });
  });

  it('return what a failed run said', async () => {
    const failed = await runEngine(request(fake(2)));
    expect(failed.exitCode).toBe(2);
    expect(failed.stderr).toBe('routing: --start is not a time\n');
    expect(failed.routesCsv).toBe('');
    expect(failed.dropsCsv).toBe('');
    expect(failed.summary).toBe('');
    const checks = await runEngine(request(fake(1)));
    expect(checks.exitCode).toBe(1);
    expect(checks.summary).toMatch(/checks: FAILED\n$/);
    expect(checks.stderr).toBe('routing: checks failed\n');
  });

  it('refuse a run outside the window, off the step, or between impossible points', async () => {
    const f = fake();
    await expect(runEngine(request(f, { start_utc: '2026-11-18T21:30:29' }))).rejects.toThrow(/outside the window/);
    await expect(runEngine(request(f, { start_utc: '2026-11-18T21:37:00', duration_s: 31 }))).rejects.toThrow(/outside the window/);
    await expect(runEngine(request(f, { duration_s: 1.5 }))).rejects.toThrow(/not a whole multiple/);
    await expect(runEngine(request(f, { start_utc: '2026-11-18 21:31:00' }))).rejects.toThrow(EngineRequestError);
    await expect(runEngine(request(f, { start_utc: '2026-02-30T21:31:00' }))).rejects.toThrow(/is not YYYY-MM-DDTHH:MM:SS/);
    await expect(runEngine(request(f, { to: { name: 'B', lat_deg: 91, lon_deg: 0 } }))).rejects.toThrow(/latitude 91/);
    await expect(runEngine(request(f, { from: { name: 'A', lat_deg: 0, lon_deg: Number.NaN } }))).rejects.toThrow(/longitude NaN/);
    await expect(runEngine(request(f, { strategies: [] }))).rejects.toThrow(/no strategies/);
    await expect(runEngine(request(f, { files: new Map() }))).rejects.toThrow(/engine\/snapshot\/gp.json was not given/);
    expect(f.calls).toEqual([]);
    expect(stepsOf(ENGINE, '2026-11-18T21:30:30', 420)).toBe(421);
  });
});

describe('names of chosen points', () => {
  it('keep letters of any script, digits and a few marks', () => {
    expect(sanitizeName('São Paulo (centro)', 'P')).toBe('São Paulo (centro)');
    expect(sanitizeName("Saint-Jean d'Acre 2", 'P')).toBe("Saint-Jean d'Acre 2");
    expect(sanitizeName('Москва', 'P')).toBe('Москва');
  });

  it('remove what a CSV field or a summary line cannot hold as it is', () => {
    expect(sanitizeName('a,b"c\nd\re\tf', 'P')).toBe('a b c d e f');
    expect(sanitizeName('A -> B', 'P')).toBe('A - B');
    expect(sanitizeName('  x   y  ', 'P')).toBe('x y');
    expect(sanitizeName(',;"', 'Point A')).toBe('Point A');
    expect(sanitizeName('', 'Point A')).toBe('Point A');
    expect(Array.from(sanitizeName('é'.repeat(60), 'P'))).toHaveLength(48);
  });

  it('make the two names different', () => {
    expect(pointNames({ name: 'Here', lat_deg: 0, lon_deg: 0 }, { name: 'Here', lat_deg: 1, lon_deg: 1 })).toEqual({ from: 'Here', to: 'Here 2' });
    expect(pointNames({ name: '', lat_deg: 0, lon_deg: 0 }, { name: ',', lat_deg: 1, lon_deg: 1 })).toEqual({ from: 'Point A', to: 'Point B' });
    const long = 'x'.repeat(60);
    const names = pointNames({ name: long, lat_deg: 0, lon_deg: 0 }, { name: long, lat_deg: 1, lon_deg: 1 });
    expect(names.from).not.toBe(names.to);
    expect(Array.from(names.to)).toHaveLength(48);
  });
});

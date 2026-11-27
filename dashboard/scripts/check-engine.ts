// The engine check (scripts/ci.sh dashboard): the exported WebAssembly routing program,
// run in Node exactly as the page runs it (src/engine/run.ts, then src/engine/from-csv.ts),
// against the native engine.
//
//  (a) The route story: the saved story's first pair, its strategies and its window, from the
//      exported files only, converted to the story's directions and compared with
//      story-route.json: every interval, route, gateway, drop, cause, element and summary
//      number equal, every delay within 1e-9 ms.
//  (b) Two chosen points in no city file, far from that pair, with the on-demand strategies,
//      against the native routing program run here with the same arguments and inputs: the
//      routes CSV equal in every column but the delays (within 1e-9 ms), the drops CSV and the
//      summary equal (the summary's "inputs:" line names the input paths, which differ; its
//      paths are replaced before comparing).
//
// Prints the largest delay difference of each part, the times (module compile, set-up,
// seconds per simulated minute) and the module's peak memory. Exit status 1 on any difference,
// 2 on a usage error.
//
// Usage, from the repository root, after `npm ci` in dashboard/ (Node 22 strips the types):
//
//     node dashboard/scripts/check-engine.ts --routing build/validate/bin/routing \
//         [--data dashboard/public/data] [--measure-longest-lived <seconds>]
//
// --measure-longest-lived runs (b)'s points once more with longest-lived added for that many
// seconds from the plan time, outside the position window, only to measure time and memory
// (not part of CI).

import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { performance } from 'node:perf_hooks';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createServer } from 'vite';
import type { EngineFactory, EnginePoint, EngineRequest, EngineResult } from '../src/engine/run';
import type { EngineManifest } from '../src/engine/settings';
import type { Direction } from '../src/stories/route/data';

type RunModule = typeof import('../src/engine/run');
type FromCsvModule = typeof import('../src/engine/from-csv');
type SettingsModule = typeof import('../src/engine/settings');
type CheckModule = typeof import('../src/data/check');
type CommonModule = typeof import('../src/data/common');
type ManifestModule = typeof import('../src/data/manifest');
type RouteDataModule = typeof import('../src/stories/route/data');

const DELAY_TOLERANCE_MS = 1e-9;
const NUMERIC_COLUMNS = new Set(['one_way_ms', 'ideal_ms', 'backup_ms']);
const DASHBOARD = fileURLToPath(new URL('..', import.meta.url));
const REPO = resolve(DASHBOARD, '..');
const CUSTOM: [EnginePoint, EnginePoint] = [
  { name: 'Chosen point 64.1 N 21.9 W', lat_deg: 64.1, lon_deg: -21.9 },
  { name: 'Chosen point 43.1 N 131.9 E', lat_deg: 43.1, lon_deg: 131.9 },
];
const MAX_REPORTED = 20;

class Differences {
  readonly found: string[] = [];
  maxDelayMs = 0;

  add(where: string, what: string): void {
    this.found.push(`${where}: ${what}`);
  }

  delay(where: string, a: number | null, b: number | null): void {
    if (a === null || b === null) {
      if (a !== b) this.add(where, `wasm ${String(a)}, expected ${String(b)}`);
      return;
    }
    const d = Math.abs(a - b);
    this.maxDelayMs = Math.max(this.maxDelayMs, d);
    if (!(d <= DELAY_TOLERANCE_MS)) this.add(where, `wasm ${a}, expected ${b} (differs by ${d} ms)`);
  }

  /** Equal values, recursively; numbers in a `delay_ms` list within the tolerance. */
  same(where: string, a: unknown, b: unknown, delays = false): void {
    if (Array.isArray(a) && Array.isArray(b)) {
      if (a.length !== b.length) {
        this.add(where, `wasm ${a.length} items, expected ${b.length}`);
        return;
      }
      a.forEach((x, i) => {
        this.same(`${where}[${i}]`, x, b[i], delays);
      });
      return;
    }
    if (delays && (typeof a === 'number' || a === null) && (typeof b === 'number' || b === null)) {
      this.delay(where, a, b);
      return;
    }
    if (typeof a === 'object' && a !== null && typeof b === 'object' && b !== null && !Array.isArray(a) && !Array.isArray(b)) {
      const ao = a as Record<string, unknown>;
      const bo = b as Record<string, unknown>;
      const keys = [...new Set([...Object.keys(ao), ...Object.keys(bo)])].sort();
      for (const key of keys) {
        if (!(key in ao) || !(key in bo)) this.add(`${where}.${key}`, `present only in ${key in ao ? 'wasm' : 'expected'}`);
        else this.same(`${where}.${key}`, ao[key], bo[key], key === 'delay_ms');
      }
      return;
    }
    if (!Object.is(a, b)) this.add(where, `wasm ${JSON.stringify(a)}, expected ${JSON.stringify(b)}`);
  }

  report(part: string): boolean {
    for (const d of this.found.slice(0, MAX_REPORTED)) console.log(`  difference: ${d}`);
    if (this.found.length > MAX_REPORTED) console.log(`  ... and ${this.found.length - MAX_REPORTED} more`);
    console.log(`  ${part}: ${this.found.length} differences, largest delay difference ${this.maxDelayMs.toExponential(2)} ms`);
    return this.found.length === 0;
  }
}

function argument(argv: string[], flag: string): string | undefined {
  const i = argv.indexOf(flag);
  if (i < 0) return undefined;
  const value = argv[i + 1];
  if (value === undefined) usage(`${flag} needs a value`);
  return value;
}

function usage(problem: string): never {
  console.error(`check-engine: ${problem}`);
  console.error('usage: node dashboard/scripts/check-engine.ts --routing <native routing> [--data <dir>] [--measure-longest-lived <s>]');
  process.exit(2);
}

/** A file's bytes in an ArrayBuffer of their own (WebAssembly.compile takes no shared one). */
function bytes(path: string): Uint8Array<ArrayBuffer> {
  const content = readFileSync(path);
  const out = new Uint8Array(content.length);
  out.set(content);
  return out;
}

interface Timed {
  result: EngineResult;
  setupS: number;
  steppingS: number;
}

async function timedRun(run: RunModule, request: EngineRequest): Promise<Timed> {
  const began = performance.now();
  let started = began;
  const result = await run.runEngine({
    ...request,
    onProgress: (done) => {
      if (done === 0) started = performance.now();
    },
  });
  const ended = performance.now();
  return { result, setupS: (started - began) / 1000, steppingS: (ended - started) / 1000 };
}

function timing(label: string, t: Timed, durationS: number): void {
  const perMinute = t.steppingS / (durationS / 60);
  console.log(
    `  ${label}: set-up ${t.setupS.toFixed(2)} s, stepping ${t.steppingS.toFixed(2)} s for ${durationS} s simulated ` +
      `(${perMinute.toFixed(2)} s per simulated minute), peak memory ${(t.result.memoryBytes / 2 ** 20).toFixed(0)} MiB`,
  );
}

function succeeded(label: string, result: EngineResult): boolean {
  if (result.exitCode === 0) return true;
  console.log(`  ${label}: the WebAssembly program exited with ${result.exitCode}:\n${result.stderr}`);
  return false;
}

/** The summary with the input paths of its "inputs:" line replaced. */
function withoutPaths(summary: string): string {
  return summary.replace(/^( {2}inputs: snapshot )\S+, ground \S+, cities \S+,/m, '$1<snapshot>, ground <ground>, cities <cities>,');
}

async function main(): Promise<number> {
  const argv = process.argv.slice(2);
  const routing = argument(argv, '--routing') ?? usage('--routing is required');
  const data = resolve(argument(argv, '--data') ?? join(REPO, 'dashboard/public/data'));
  const longest = argument(argv, '--measure-longest-lived');

  const server = await createServer({
    root: DASHBOARD,
    configFile: false,
    logLevel: 'error',
    appType: 'custom',
    server: { middlewareMode: true, hmr: false, ws: false },
  });
  try {
    const load = async <T>(path: string): Promise<T> => (await server.ssrLoadModule(path)) as unknown as T;
    const run = await load<RunModule>('/src/engine/run.ts');
    const fromCsv = await load<FromCsvModule>('/src/engine/from-csv.ts');
    const settings = await load<SettingsModule>('/src/engine/settings.ts');
    const checks = await load<CheckModule>('/src/data/check.ts');
    const common = await load<CommonModule>('/src/data/common.ts');
    const manifests = await load<ManifestModule>('/src/data/manifest.ts');
    const routeData = await load<RouteDataModule>('/src/stories/route/data.ts');

    const json = (name: string): unknown => JSON.parse(readFileSync(join(data, name), 'utf8')) as unknown;
    const manifestJson = json('manifest.json');
    const manifest = manifests.parseManifest(manifestJson, new checks.Check('manifest.json'));
    const engine: EngineManifest = settings.parseEngine(manifestJson, new checks.Check('manifest.json'));
    const satellites = common.parseSatellites(json('satellites.json'), new checks.Check('satellites.json'), manifest.satellites);
    const sites = common.parseSites(json('sites.json'), new checks.Check('sites.json'));
    const story = routeData.parseRouteStory(json('story-route.json'), new checks.Check('story-route.json'), manifest.satellites, sites, manifest.settings.window_s);

    const index = (kind: string): Map<string, number> =>
      new Map(sites.flatMap((s, i) => (s.kind === kind ? [[s.name, i] as [string, number]] : [])));
    const cityIndex = index('city');
    const gatewayIndex = index('gateway');
    const satelliteIndex = new Map(satellites.norad.map((n, i) => [n, i]));
    const lookups = {
      satellite: (n: number) => satelliteIndex.get(n),
      city: (name: string) => cityIndex.get(name),
      gateway: (name: string) => gatewayIndex.get(name),
    };
    const files = new Map([...engine.snapshot, ...engine.ground].map((p) => [p, bytes(join(data, p))]));
    const factory = ((await import(pathToFileURL(join(data, engine.program.js)).href)) as { default: EngineFactory }).default;
    let began = performance.now();
    const wasm = await WebAssembly.compile(bytes(join(data, engine.program.wasm)));
    console.log(`engine check: module compiled in ${((performance.now() - began) / 1000).toFixed(2)} s`);
    const base = { factory, wasm, files, engine };
    let passed = true;

    // (a) The route story's first pair.
    const first = story.directions[0];
    if (first === undefined) throw new Error('story-route.json has no direction');
    const known = (JSON.parse(readFileSync(join(REPO, 'data/ground/cities.json'), 'utf8')) as { cities: { name: string; country: string; region: string; geofeed_city: string }[] }).cities;
    const point = (i: number): EnginePoint => {
      const site = sites[i];
      if (site === undefined) throw new Error(`site ${i} is not in sites.json`);
      const city = known.find((c) => c.name === site.name);
      return { name: site.name, lat_deg: site.lat_deg, lon_deg: site.lon_deg, ...(city ? { country: city.country, region: city.region, geofeed_city: city.geofeed_city } : {}) };
    };
    const { start_utc, duration_s, strategies } = story.settings;
    console.log(`(a) the route story: ${sites[first.from]?.name ?? '?'} - ${sites[first.to]?.name ?? '?'}, ${strategies.join(', ')}, ${start_utc} for ${duration_s} s`);
    const a = await timedRun(run, { ...base, from: point(first.from), to: point(first.to), start_utc, duration_s, strategies });
    timing('wasm', a, duration_s);
    const da = new Differences();
    if (succeeded('(a)', a.result)) {
      const { from, to } = a.result.names;
      const converted: Direction[] = fromCsv.directionsFromCsv(a.result, { directions: [[from, to], [to, from]], strategies, start_utc, duration_s }, lookups);
      da.same('directions', converted, story.directions.slice(0, 2));
    } else {
      da.add('(a)', 'the run failed');
    }
    passed = da.report('(a)') && passed;

    // (b) Two chosen points, the on-demand strategies, against the native program.
    const [p, q] = CUSTOM;
    console.log(`(b) chosen points: ${p.name} - ${q.name}, ${engine.strategies.join(', ')}, ${start_utc} for ${duration_s} s`);
    const b = await timedRun(run, { ...base, from: p, to: q, start_utc, duration_s });
    timing('wasm', b, duration_s);
    const db = new Differences();
    if (succeeded('(b)', b.result)) {
      const dir = mkdtempSync(join(tmpdir(), 'satsim-engine-check-'));
      try {
        writeFileSync(join(dir, 'cities.json'), b.result.citiesJson);
        const args = run.engineArguments(
          engine,
          { snapshot: join(data, 'engine/snapshot'), ground: join(data, 'engine/ground'), cities: join(dir, 'cities.json'), drops: join(dir, 'drops.csv') },
          start_utc,
          duration_s,
          engine.strategies,
        );
        began = performance.now();
        const native = spawnSync(routing, args, { encoding: 'utf8', maxBuffer: 1 << 30 });
        console.log(`  native: ${((performance.now() - began) / 1000).toFixed(2)} s in all, exit status ${String(native.status)}`);
        if (native.status !== 0) {
          db.add('native', `exit status ${String(native.status)}: ${native.stderr}`);
        } else {
          // With --out -, the routes CSV goes to standard output and the summary to standard error.
          const ours = fromCsv.parseCsv(b.result.routesCsv, fromCsv.ROUTES_COLUMNS, 'wasm routes CSV');
          const theirs = fromCsv.parseCsv(native.stdout, fromCsv.ROUTES_COLUMNS, 'native routes CSV');
          if (ours.length !== theirs.length) db.add('routes CSV', `wasm ${ours.length} rows, native ${theirs.length}`);
          ours.forEach((row, k) => {
            const other = theirs[k];
            if (other === undefined) return;
            for (const column of fromCsv.ROUTES_COLUMNS) {
              const x = row[column] ?? '';
              const y = other[column] ?? '';
              if (NUMERIC_COLUMNS.has(column) && x !== '' && y !== '') db.delay(`routes row ${k + 1} ${column}`, Number(x), Number(y));
              else if (x !== y) db.add(`routes row ${k + 1} ${column}`, `wasm "${x}", native "${y}"`);
            }
          });
          if (b.result.dropsCsv !== readFileSync(join(dir, 'drops.csv'), 'utf8')) db.add('drops CSV', 'differs');
          if (withoutPaths(b.result.summary) !== withoutPaths(native.stderr)) {
            const w = withoutPaths(b.result.summary).split('\n');
            const n = withoutPaths(native.stderr).split('\n');
            const line = w.findIndex((l, i) => l !== n[i]);
            db.add('summary', `line ${line + 1}: wasm "${w[line] ?? ''}", native "${n[line] ?? ''}"`);
          }
          console.log(`  rows compared: ${ours.length}, drops: ${b.result.dropsCsv.split('\n').length - 2}`);
        }
      } finally {
        rmSync(dir, { recursive: true, force: true });
      }
    } else {
      db.add('(b)', 'the run failed');
    }
    passed = db.report('(b)') && passed;

    if (longest !== undefined) {
      const seconds = Number(longest);
      const wide = { ...engine, window: { start_utc: engine.settings.plan_time_utc, duration_s: seconds } };
      const all = [...engine.strategies, 'longest-lived'];
      console.log(`(measure) ${all.join(', ')}, ${engine.settings.plan_time_utc} for ${seconds} s`);
      const m = await timedRun(run, { ...base, engine: wide, from: p, to: q, start_utc: engine.settings.plan_time_utc, duration_s: seconds, strategies: all });
      succeeded('(measure)', m.result);
      timing('wasm', m, seconds);
    }
    console.log(`engine check: ${passed ? 'PASSED' : 'FAILED'}`);
    return passed ? 0 : 1;
  } finally {
    await server.close();
  }
}

process.exit(await main());

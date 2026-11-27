// One run of the routing program in WebAssembly (apps/routing_wasm) between two points, with
// the route story's settings (the manifest's engine block). No DOM and no fetch: the caller
// passes the module factory and every file as bytes, so a page, a Web Worker and Node (the
// engine check, scripts/check-engine.ts) all run it the same way. Every run gets a fresh
// module instance, so nothing of one run leaks into the next.

import type { EngineManifest, Placeholder } from './settings';

/** A point to route between: a saved city, or one chosen on the globe. */
export interface EnginePoint {
  name: string;
  lat_deg: number;
  lon_deg: number;
  /** Of a city in data/ground/cities.json; a chosen point has none (ZZ, as Hypatia's cities). */
  country?: string;
  region?: string;
  geofeed_city?: string;
}

/** The part of Emscripten's module the run uses. */
export interface EngineModule {
  FS: {
    mkdirTree(path: string): void;
    writeFile(path: string, data: Uint8Array | string): void;
    readFile(path: string, options: { encoding: 'utf8' }): string;
    analyzePath(path: string): { exists: boolean };
  };
  callMain(args: string[]): number;
  HEAPU8: Uint8Array;
}

export interface EngineModuleOptions {
  wasmBinary?: Uint8Array;
  instantiateWasm?: (
    imports: WebAssembly.Imports,
    receive: (instance: WebAssembly.Instance, module: WebAssembly.Module) => void,
  ) => object;
  print: (text: string) => void;
  printErr: (text: string) => void;
  /** Called by apps/routing_wasm with the number of routes CSV lines written so far. */
  onRoutesLine: (lines: number) => void;
}

/** The default export of satsim-routing.js. */
export type EngineFactory = (options: EngineModuleOptions) => Promise<EngineModule>;

export interface EngineRequest {
  factory: EngineFactory;
  /** satsim-routing.wasm, as bytes or compiled once by the caller. */
  wasm: Uint8Array | WebAssembly.Module;
  /** Every file of engine.snapshot and engine.ground, by its path in the export. */
  files: ReadonlyMap<string, Uint8Array>;
  engine: EngineManifest;
  from: EnginePoint;
  to: EnginePoint;
  /** YYYY-MM-DDTHH:MM:SS, UTC. */
  start_utc: string;
  duration_s: number;
  /** Default: engine.strategies. */
  strategies?: readonly string[];
  /** After each completed time step (0 when the program starts stepping). */
  onProgress?: (stepsDone: number, stepsTotal: number) => void;
}

export interface EngineResult {
  routesCsv: string;
  dropsCsv: string;
  summary: string;
  exitCode: number;
  stderr: string;
  /** The names the run used for the two points (sanitised and unique). */
  names: { from: string; to: string };
  /** The one-pair cities file the run used. */
  citiesJson: string;
  /** The module's memory after the run, which is its peak: WebAssembly memory never shrinks. */
  memoryBytes: number;
}

/** A request the program would refuse, refused before it starts. */
export class EngineRequestError extends Error {
  constructor(problem: string) {
    super(problem);
    this.name = 'EngineRequestError';
  }
}

const ROOT = '/data';
const RUN = '/run';
const MAX_NAME_LENGTH = 48;
const UTC = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})$/;

/**
 * A name the engine's files can hold as it is: letters and digits of any script, spaces and
 * . - ' ( ); anything else (commas, quotes, line ends, ->) becomes a space, runs of spaces
 * one, at most 48 characters; `fallback` if nothing is left.
 */
export function sanitizeName(raw: string, fallback: string): string {
  const kept = raw.normalize('NFC').replace(/[^\p{L}\p{N}\p{M} .'()-]/gu, ' ');
  const collapsed = kept.replace(/ +/g, ' ').trim();
  const cut = Array.from(collapsed).slice(0, MAX_NAME_LENGTH).join('').trim();
  return cut === '' ? fallback : cut;
}

/** The two names a run uses: sanitised, and different (the second gets " 2" if needed). */
export function pointNames(from: EnginePoint, to: EnginePoint): { from: string; to: string } {
  const a = sanitizeName(from.name, 'Point A');
  let b = sanitizeName(to.name, 'Point B');
  if (b === a) b = `${Array.from(b).slice(0, MAX_NAME_LENGTH - 2).join('')} 2`;
  return { from: a, to: b };
}

function checkPoint(p: EnginePoint, which: string): void {
  if (!Number.isFinite(p.lat_deg) || Math.abs(p.lat_deg) > 90) {
    throw new EngineRequestError(`${which}: latitude ${p.lat_deg} is not between -90 and 90`);
  }
  if (!Number.isFinite(p.lon_deg) || Math.abs(p.lon_deg) > 180) {
    throw new EngineRequestError(`${which}: longitude ${p.lon_deg} is not between -180 and 180`);
  }
}

/**
 * The cities file of one pair, in the shape python/run_phase4.py write_pair_cities writes:
 * the two cities and the pair from-to with role study.
 */
export function citiesFile(from: EnginePoint, to: EnginePoint): string {
  checkPoint(from, 'from');
  checkPoint(to, 'to');
  const names = pointNames(from, to);
  const city = (p: EnginePoint, name: string): Record<string, unknown> => ({
    name,
    country: p.country ?? 'ZZ',
    region: p.region ?? 'ZZ',
    geofeed_city: p.geofeed_city ?? name,
    lat: p.lat_deg,
    lon: p.lon_deg,
  });
  const content = {
    note: `One pair for the dashboard's on-demand route: ${names.from} to ${names.to}.`,
    cities: [city(from, names.from), city(to, names.to)],
    pairs: [{ from: names.from, to: names.to, role: 'study', why: 'chosen on the dashboard' }],
  };
  return `${JSON.stringify(content, null, 1)}\n`;
}

/** Seconds since 1970 of YYYY-MM-DDTHH:MM:SS (UTC), or null if it is not that form. */
export function utcSeconds(text: string): number | null {
  const m = UTC.exec(text);
  if (m === null) return null;
  const [y, mo, d, h, mi, s] = m.slice(1).map(Number) as [number, number, number, number, number, number];
  const ms = Date.UTC(y, mo - 1, d, h, mi, s);
  const back = new Date(ms).toISOString().slice(0, 19);
  return back === text ? ms / 1000 : null;
}

/** The time window and step checks; returns the number of time steps. */
export function stepsOf(engine: EngineManifest, start_utc: string, duration_s: number): number {
  const start = utcSeconds(start_utc);
  const windowStart = utcSeconds(engine.window.start_utc);
  if (start === null) throw new EngineRequestError(`start ${start_utc} is not YYYY-MM-DDTHH:MM:SS`);
  if (windowStart === null) throw new EngineRequestError(`the window start ${engine.window.start_utc} is not a time`);
  const step = engine.settings.step_s;
  if (!Number.isInteger(duration_s) || duration_s < 0 || duration_s % step !== 0) {
    throw new EngineRequestError(`duration ${duration_s} s is not a whole multiple of the ${step} s step`);
  }
  const windowEnd = windowStart + engine.window.duration_s;
  if (start < windowStart || start + duration_s > windowEnd) {
    throw new EngineRequestError(
      `the run from ${start_utc} for ${duration_s} s is outside the window ` +
        `${engine.window.start_utc} + ${engine.window.duration_s} s`,
    );
  }
  return duration_s / step + 1;
}

/** Where a run's files are, for the argument list. */
export interface RunPaths {
  snapshot: string;
  ground: string;
  cities: string;
  drops: string;
}

/** engine.arguments with every placeholder filled in. */
export function engineArguments(
  engine: EngineManifest,
  paths: RunPaths,
  start_utc: string,
  duration_s: number,
  strategies: readonly string[],
): string[] {
  if (strategies.length === 0) throw new EngineRequestError('no strategies');
  for (const s of strategies) {
    if (s === '' || s.includes(',')) throw new EngineRequestError(`strategy "${s}" is not a name`);
  }
  const values: Record<Placeholder, string> = {
    '{snapshot}': paths.snapshot,
    '{ground}': paths.ground,
    '{cities}': paths.cities,
    '{start_utc}': start_utc,
    '{duration_s}': String(duration_s),
    '{strategies}': strategies.join(','),
    '{drops}': paths.drops,
  };
  return engine.arguments.map((a) => (Object.hasOwn(values, a) ? values[a as Placeholder] : a));
}

function baseName(path: string): string {
  return path.slice(path.lastIndexOf('/') + 1);
}

function moduleOptions(request: EngineRequest, onLine: (lines: number) => void, log: string[]): EngineModuleOptions {
  const common = { print: (t: string) => log.push(t), printErr: (t: string) => log.push(t), onRoutesLine: onLine };
  const wasm = request.wasm;
  if (wasm instanceof Uint8Array) return { ...common, wasmBinary: wasm };
  return {
    ...common,
    instantiateWasm: (imports, receive) => {
      void WebAssembly.instantiate(wasm, imports).then((instance) => {
        receive(instance, wasm);
      });
      return {};
    },
  };
}

/** Runs the routing program once; throws EngineRequestError before starting on a bad request. */
export async function runEngine(request: EngineRequest): Promise<EngineResult> {
  const { engine } = request;
  const strategies = request.strategies ?? engine.strategies;
  const stepsTotal = stepsOf(engine, request.start_utc, request.duration_s);
  const citiesJson = citiesFile(request.from, request.to);
  const names = pointNames(request.from, request.to);
  const paths: RunPaths = {
    snapshot: `${ROOT}/snapshot`,
    ground: `${ROOT}/ground`,
    cities: `${ROOT}/cities.json`,
    drops: `${RUN}/drops.csv`,
  };
  const args = engineArguments(engine, paths, request.start_utc, request.duration_s, strategies);
  const inputs: [string, Uint8Array][] = [];
  for (const [dir, list] of [
    [paths.snapshot, engine.snapshot],
    [paths.ground, engine.ground],
  ] as const) {
    for (const path of list) {
      const bytes = request.files.get(path);
      if (bytes === undefined) throw new EngineRequestError(`the input ${path} was not given`);
      inputs.push([`${dir}/${baseName(path)}`, bytes]);
    }
  }

  // Rows of the routes CSV: a header, then per step one per direction (both) and strategy.
  const rowsPerStep = 2 * strategies.length;
  let reported = -1;
  const onLine = (lines: number): void => {
    const done = Math.min(Math.floor(Math.max(lines - 1, 0) / rowsPerStep), stepsTotal);
    if (done > reported) {
      reported = done;
      request.onProgress?.(done, stepsTotal);
    }
  };
  const log: string[] = [];
  const module = await request.factory(moduleOptions(request, onLine, log));
  const fs = module.FS;
  fs.mkdirTree(paths.snapshot);
  fs.mkdirTree(paths.ground);
  fs.mkdirTree(RUN);
  for (const [path, bytes] of inputs) fs.writeFile(path, bytes);
  fs.writeFile(paths.cities, citiesJson);

  const exitCode = module.callMain(args);
  const read = (path: string): string => (fs.analyzePath(path).exists ? fs.readFile(path, { encoding: 'utf8' }) : '');
  const messages = read(`${RUN}/messages.txt`);
  return {
    routesCsv: read(`${RUN}/routes.csv`),
    dropsCsv: read(paths.drops),
    summary: read(`${RUN}/summary.txt`),
    exitCode,
    stderr: messages + log.map((line) => `${line}\n`).join(''),
    names,
    citiesJson,
    memoryBytes: module.HEAPU8.length,
  };
}

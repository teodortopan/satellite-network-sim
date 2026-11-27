// manifest.json (dashboard/data-format.md).

import { parseEngine, type EngineManifest } from '../engine/settings';
import type { Check } from './check';

export interface FileEntry {
  path: string;
  bytes: number;
  sha256: string;
}

export interface PositionFile {
  path: string;
  first_sample: number;
  samples: number;
}

export interface Manifest {
  snapshot: string;
  ground: string;
  settings: {
    mode: 'small' | 'full';
    plan_time_utc: string;
    window_start_utc: string;
    window_s: number;
    sample_step_s: number;
    samples: number;
  };
  satellites: number;
  positions: PositionFile[];
  files: FileEntry[];
  /** The routing program in WebAssembly and its inputs (data-format.md "The engine"). */
  engine: EngineManifest;
}

export const FORMAT_VERSION = 1;
const FLOAT_BYTES = 4;

export function parseManifest(value: unknown, c: Check): Manifest {
  const root = c.object(value, '');
  const version = c.integer(c.field(root, 'format_version', ''), 'format_version');
  if (version !== FORMAT_VERSION) {
    c.fail('format_version', `is ${version}; this page reads format ${FORMAT_VERSION}`);
  }
  const s = c.object(c.field(root, 'settings', ''), 'settings');
  const settings = {
    mode: c.oneOf(c.field(s, 'mode', 'settings'), ['small', 'full'] as const, 'settings.mode'),
    plan_time_utc: c.string(c.field(s, 'plan_time_utc', 'settings'), 'settings.plan_time_utc'),
    window_start_utc: c.string(
      c.field(s, 'window_start_utc', 'settings'),
      'settings.window_start_utc',
    ),
    window_s: c.integer(c.field(s, 'window_s', 'settings'), 'settings.window_s'),
    sample_step_s: c.integer(c.field(s, 'sample_step_s', 'settings'), 'settings.sample_step_s'),
    samples: c.integer(c.field(s, 'samples', 'settings'), 'settings.samples'),
  };
  if (settings.sample_step_s <= 0 || settings.samples < 2) {
    c.fail('settings', 'should have a positive sample step and at least two samples');
  }
  if (settings.window_s !== (settings.samples - 1) * settings.sample_step_s) {
    c.fail('settings.window_s', 'does not match the samples and the sample step');
  }
  const satellites = c.integer(c.field(root, 'satellites', ''), 'satellites');
  if (satellites <= 0) c.fail('satellites', 'should be at least 1');

  const files = c.list(c.field(root, 'files', ''), 'files', (item, path) => {
    const f = c.object(item, path);
    const bytes = c.integer(c.field(f, 'bytes', path), `${path}.bytes`);
    if (bytes < 0) c.fail(`${path}.bytes`, 'should not be negative');
    return {
      path: c.string(c.field(f, 'path', path), `${path}.path`),
      bytes,
      sha256: c.string(c.field(f, 'sha256', path), `${path}.sha256`),
    };
  });

  let next = 0;
  const positions = c.list(c.field(root, 'positions', ''), 'positions', (item, path) => {
    const p = c.object(item, path);
    const entry = {
      path: c.string(c.field(p, 'path', path), `${path}.path`),
      first_sample: c.integer(c.field(p, 'first_sample', path), `${path}.first_sample`),
      samples: c.integer(c.field(p, 'samples', path), `${path}.samples`),
    };
    if (entry.first_sample !== next || entry.samples <= 0) {
      c.fail(path, `should start at sample ${next} and hold at least one sample`);
    }
    next += entry.samples;
    const listed = files.find((f) => f.path === entry.path);
    if (listed === undefined) c.fail(`${path}.path`, `"${entry.path}" is not in files`);
    if (listed.bytes !== entry.samples * satellites * 3 * FLOAT_BYTES) {
      c.fail(path, `holds ${entry.samples} samples, which does not match its size`);
    }
    return entry;
  });
  if (next !== settings.samples) {
    c.fail('positions', `cover ${next} samples, but settings.samples is ${settings.samples}`);
  }

  const engine = parseEngine(value, c);
  for (const path of [engine.program.js, engine.program.wasm, ...engine.snapshot, ...engine.ground]) {
    if (!files.some((f) => f.path === path)) c.fail('engine', `names "${path}", which is not in files`);
  }

  return {
    snapshot: c.string(c.field(root, 'snapshot', ''), 'snapshot'),
    ground: c.string(c.field(root, 'ground', ''), 'ground'),
    settings,
    satellites,
    positions,
    files,
    engine,
  };
}

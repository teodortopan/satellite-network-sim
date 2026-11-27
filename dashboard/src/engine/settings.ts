// The manifest's `engine` block (dashboard/data-format.md "The engine"): where the routing
// program in WebAssembly and its inputs are, and the settings of the route story it runs with.

import type { Check, JsonObject } from '../data/check';

/** What a run fills into the argument list; every one must appear exactly once. */
export const PLACEHOLDERS = [
  '{snapshot}',
  '{ground}',
  '{cities}',
  '{start_utc}',
  '{duration_s}',
  '{strategies}',
  '{drops}',
] as const;
export type Placeholder = (typeof PLACEHOLDERS)[number];

export interface EngineManifest {
  /** Paths in the export of the ES module and the WebAssembly binary. */
  program: { js: string; wasm: string };
  /** Paths in the export of the snapshot's and the ground data's files, as the program reads them. */
  snapshot: string[];
  ground: string[];
  settings: {
    model: string;
    pattern: string;
    replan_s: number;
    planner: string;
    backup: string;
    step_s: number;
    plan_time_utc: string;
    roles: string;
  };
  /** The strategies a run between two chosen points uses. */
  strategies: string[];
  /** A run must lie inside it: the position window. */
  window: { start_utc: string; duration_s: number };
  /** The routing program's arguments, with the placeholders for what a run chooses. */
  arguments: string[];
}

/** The `engine` field of manifest.json's root object. */
export function parseEngine(manifestRoot: unknown, c: Check): EngineManifest {
  const root = c.object(manifestRoot, '');
  const e = c.object(c.field(root, 'engine', ''), 'engine');
  const get = (o: JsonObject, key: string, path: string): unknown => c.field(o, key, path);
  const str = (o: JsonObject, key: string, path: string): string =>
    c.string(get(o, key, path), `${path}.${key}`);
  const paths = (key: string): string[] => {
    const list = c.list(get(e, key, 'engine'), `engine.${key}`, (v, p) => c.string(v, p));
    if (list.length === 0) c.fail(`engine.${key}`, 'is empty');
    return list;
  };

  const p = c.object(get(e, 'program', 'engine'), 'engine.program');
  const s = c.object(get(e, 'settings', 'engine'), 'engine.settings');
  const settings = {
    model: str(s, 'model', 'engine.settings'),
    pattern: str(s, 'pattern', 'engine.settings'),
    replan_s: c.number(get(s, 'replan_s', 'engine.settings'), 'engine.settings.replan_s'),
    planner: str(s, 'planner', 'engine.settings'),
    backup: str(s, 'backup', 'engine.settings'),
    step_s: c.integer(get(s, 'step_s', 'engine.settings'), 'engine.settings.step_s'),
    plan_time_utc: str(s, 'plan_time_utc', 'engine.settings'),
    roles: str(s, 'roles', 'engine.settings'),
  };
  if (settings.step_s <= 0) c.fail('engine.settings.step_s', 'should be positive');
  const w = c.object(get(e, 'window', 'engine'), 'engine.window');
  const window = {
    start_utc: str(w, 'start_utc', 'engine.window'),
    duration_s: c.integer(get(w, 'duration_s', 'engine.window'), 'engine.window.duration_s'),
  };
  if (window.duration_s < 0) c.fail('engine.window.duration_s', 'should not be negative');
  const args = c.list(get(e, 'arguments', 'engine'), 'engine.arguments', (v, q) => c.string(v, q));
  for (const placeholder of PLACEHOLDERS) {
    const n = args.filter((a) => a === placeholder).length;
    if (n !== 1) c.fail('engine.arguments', `should hold ${placeholder} once, not ${n} times`);
  }
  return {
    program: { js: str(p, 'js', 'engine.program'), wasm: str(p, 'wasm', 'engine.program') },
    snapshot: paths('snapshot'),
    ground: paths('ground'),
    settings,
    strategies: paths('strategies'),
    window,
    arguments: args,
  };
}

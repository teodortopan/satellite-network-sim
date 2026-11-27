import { describe, expect, it } from 'vitest';
import { Check, DataError } from '../data/check';
import { parseEngine } from './settings';

function manifest(): { engine: Record<string, unknown> } {
  return {
    engine: {
      program: { js: 'engine/satsim-routing.js', wasm: 'engine/satsim-routing.wasm' },
      snapshot: ['engine/snapshot/gp.json'],
      ground: ['engine/ground/gateways.json'],
      settings: { model: 'best-case', pattern: 'three', replan_s: 15, planner: 'start', backup: 'on', step_s: 1, plan_time_utc: '2026-11-18T21:15:00', roles: 'study' },
      strategies: ['lowest-delay'],
      window: { start_utc: '2026-11-18T21:30:30', duration_s: 420 },
      arguments: ['--snapshot', '{snapshot}', '--ground', '{ground}', '--cities', '{cities}', '--start', '{start_utc}', '--duration-s', '{duration_s}', '--strategies', '{strategies}', '--drops-out', '{drops}'],
    },
  };
}

function problem(value: unknown): string {
  try {
    parseEngine(value, new Check('manifest.json'));
  } catch (error) {
    if (error instanceof DataError) return error.problem;
    throw error;
  }
  return 'accepted';
}

describe('the manifest engine block', () => {
  it('read a good block', () => {
    const engine = parseEngine(manifest(), new Check('manifest.json'));
    expect(engine.program.wasm).toBe('engine/satsim-routing.wasm');
    expect(engine.window).toEqual({ start_utc: '2026-11-18T21:30:30', duration_s: 420 });
    expect(engine.settings.replan_s).toBe(15);
  });

  it('refuse a missing block, field or placeholder', () => {
    expect(problem({})).toBe('engine is missing');
    const noStep = manifest();
    delete (noStep.engine.settings as Record<string, unknown>).step_s;
    expect(problem(noStep)).toBe('engine.settings.step_s is missing');
    const noDrops = manifest();
    noDrops.engine.arguments = (noDrops.engine.arguments as string[]).slice(0, -1);
    expect(problem(noDrops)).toBe('engine.arguments should hold {drops} once, not 0 times');
    const twice = manifest();
    twice.engine.arguments = [...(twice.engine.arguments as string[]), '{cities}'];
    expect(problem(twice)).toBe('engine.arguments should hold {cities} once, not 2 times');
    const empty = manifest();
    empty.engine.strategies = [];
    expect(problem(empty)).toBe('engine.strategies is empty');
  });
});

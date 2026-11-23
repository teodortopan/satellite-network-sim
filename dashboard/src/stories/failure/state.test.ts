import { describe, expect, it } from 'vitest';
import { Check } from '../../data/check';
import { parseFailureStory, type Interval } from './data';
import { SITES, storyJson } from './fixture';
import { drawnPath, factAt, firstKnownFact, intervalAt, newPlanAt, satelliteDetections } from './state';

const scenario = parseFailureStory(storyJson(), new Check('story-failure.json'), 4, SITES, 420).scenarios[0];
if (scenario === undefined) throw new Error('fixture has no scenario');
const lane = scenario.lanes.planner;

function iv(state: Interval['state'], route: number[], failed: Interval['failed_element'], gateway: number | null): Interval {
  return { start_s: 0, end_s: 1, state, drop_cause: null, failed_element: failed, one_way_ms: null, route, serving: null, gateway };
}

describe('the state at time t', () => {
  it('finds the interval [start, end) holding t', () => {
    expect(intervalAt(lane, 0)?.state).toBe('delivered');
    expect(intervalAt(lane, 9.999)?.state).toBe('delivered');
    expect(intervalAt(lane, 10)?.state).toBe('black_hole');
    expect(intervalAt(lane, 11.5)?.state).toBe('black_hole');
    expect(intervalAt(lane, 12)?.state).toBe('delivered');
    expect(intervalAt(lane, 12)?.route).toEqual([0, 2, 3]);
  });

  it('keeps the last interval at the run end and nothing outside the run', () => {
    expect(intervalAt(lane, 20)?.start_s).toBe(12);
    expect(intervalAt(lane, 20.001)).toBeNull();
    expect(intervalAt(lane, -0.001)).toBeNull();
    expect(intervalAt([], 0)).toBeNull();
  });

  it('knows what each actor knows of a fact at t', () => {
    const fact = scenario.facts[0];
    if (fact === undefined) throw new Error('no fact');
    expect(factAt(fact, 9.99)).toEqual({ happened: false, detected: false, plannerKnows: false, floodDone: false });
    expect(factAt(fact, 10.05)).toEqual({ happened: true, detected: true, plannerKnows: true, floodDone: false });
    expect(factAt(fact, 10.2)).toEqual({ happened: true, detected: true, plannerKnows: true, floodDone: true });
  });

  it('finds when the planner knew and when the new plan takes effect', () => {
    const fact = firstKnownFact(scenario.facts);
    expect(fact?.planner_known_s).toBe(10.05);
    expect(newPlanAt(scenario.plans.planner, 10.05)).toBe(15);
    expect(newPlanAt(scenario.plans.planner, 0)).toBe(0);
    expect(newPlanAt(scenario.plans.planner, 15.01)).toBeNull();
    expect(newPlanAt(scenario.plans.planner, null)).toBeNull();
  });

  it('marks each detecting satellite from its detection instant', () => {
    const marks = satelliteDetections(scenario.facts, 4, Infinity);
    expect(marks[0]).toBeCloseTo(10.1, 5);
    expect(marks[1]).toBe(Infinity);
  });
});

describe('the route as drawn', () => {
  const site = (s: number): { kind: 'site'; site: number } => ({ kind: 'site', site: s });
  const sat = (s: number): { kind: 'sat'; sat: number } => ({ kind: 'sat', sat: s });

  it('delivered: up from the city, through the satellites, down to the gateway, on by fibre', () => {
    expect(drawnPath(iv('delivered', [0, 1], null, 2), 0, 1, 3)).toEqual({
      space: [site(0), sat(0), sat(1), site(2)],
      ground: [2, 3, 1],
      lost: null,
    });
  });

  it('delivered without a gateway (best case): down to the receiving city', () => {
    expect(drawnPath(iv('delivered', [0], null, null), 0, 1, 3).space).toEqual([site(0), sat(0), site(1)]);
  });

  it('black hole: up to the failed satellite on the held route', () => {
    expect(drawnPath(iv('black_hole', [0, 1, 2], { kind: 'satellite', sat: 1 }, 2), 0, 1, 3)).toEqual({
      space: [site(0), sat(0), sat(1)],
      ground: [],
      lost: sat(1),
    });
  });

  it('black hole: a walk that stops before the dead element ends at it', () => {
    expect(drawnPath(iv('black_hole', [0], { kind: 'satellite', sat: 1 }, null), 0, 1, 3).space).toEqual([site(0), sat(0), sat(1)]);
    expect(drawnPath(iv('black_hole', [0, 1], { kind: 'gateway', site: 2 }, 2), 0, 1, 3).lost).toEqual(site(2));
  });

  it('black hole: a dead link stops the route at its first end', () => {
    expect(drawnPath(iv('black_hole', [0, 1, 2], { kind: 'link', a: 2, b: 1 }, 2), 0, 1, 3).space).toEqual([site(0), sat(0), sat(1)]);
  });

  it('loop: the walk as written; no route: nothing', () => {
    expect(drawnPath(iv('loop', [0, 2, 0], null, null), 0, 1, 3).space).toEqual([site(0), sat(0), sat(2), sat(0)]);
    expect(drawnPath(iv('no_route', [], null, null), 0, 1, 3).space).toEqual([]);
  });
});

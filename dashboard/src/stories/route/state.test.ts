import { describe, expect, it } from 'vitest';
import { Check } from '../../data/check';
import { parseRouteStory, type StrategyRun } from './data';
import { SITES, storyJson } from './fixture';
import { changeSteps, dropAtStep, intervalAtStep, stateAt, stepAt } from './state';

const run = parseRouteStory(storyJson(), new Check('story-route.json'), 4, SITES, 420).directions[0]?.strategies[
  'lowest-delay'
] as StrategyRun;

describe('the route state at time t', () => {
  it('is the state of step floor(t)', () => {
    expect(stepAt(0, 21)).toBe(0);
    expect(stepAt(4.999, 21)).toBe(4);
    expect(stepAt(20, 21)).toBe(20);
    expect(stepAt(20.5, 21)).toBeNull();
    expect(stepAt(-0.1, 21)).toBeNull();
  });

  it('finds the route at the boundaries of its interval (both ends included)', () => {
    expect(stateAt(run, 4)).toMatchObject({ kind: 'route', step: 4, interval: { first_s: 0, route: [0, 1] } });
    expect(stateAt(run, 4.9)).toMatchObject({ kind: 'route', interval: { first_s: 0 } });
    expect(stateAt(run, 5)).toMatchObject({ kind: 'route', step: 5, interval: { first_s: 5, route: [1, 2] }, delay_ms: 20.5 });
    expect(stateAt(run, 9.99)).toMatchObject({ kind: 'route', interval: { first_s: 5 } });
  });

  it('finds a drop from its start to just before its end, with the route held before it', () => {
    expect(stateAt(run, 10)).toMatchObject({ kind: 'drop', step: 10, drop: { cause: 'ground_link_lost' }, broken: { first_s: 5, route: [1, 2] } });
    expect(stateAt(run, 14.99)).toMatchObject({ kind: 'drop', step: 14 });
    expect(stateAt(run, 15)).toMatchObject({ kind: 'route', interval: { first_s: 15, route: [2, 3] } });
  });

  it('has a state at both ends of the window and none outside it', () => {
    expect(stateAt(run, 0)).toMatchObject({ kind: 'route', interval: { first_s: 0 }, delay_ms: 20 });
    expect(stateAt(run, 20)).toMatchObject({ kind: 'route', interval: { first_s: 15 }, delay_ms: 22 });
    expect(stateAt(run, 20.01)).toEqual({ kind: 'none', step: null });
    expect(stateAt(run, -1)).toEqual({ kind: 'none', step: null });
  });

  it('looks up intervals and drops by binary search, including empty lists', () => {
    expect(intervalAtStep(run.intervals, 12)).toBeNull();
    expect(intervalAtStep([], 0)).toBeNull();
    expect(dropAtStep(run.drops, 9)).toBeNull();
    expect(dropAtStep(run.drops, 15)).toBeNull();
    expect(dropAtStep([], 3)).toBeNull();
    const many = Array.from({ length: 1000 }, (_, i) => ({ first_s: 2 * i, last_s: 2 * i, route: [i % 4], gateway: null }));
    expect(intervalAtStep(many, 1234)?.first_s).toBe(1234);
    expect(intervalAtStep(many, 1235)).toBeNull();
  });

  it('marks a route change only where an interval follows the one before it without a drop', () => {
    expect(changeSteps(run.intervals)).toEqual([5]);
    expect(changeSteps(run.intervals)).toHaveLength(run.summary.changes);
  });
});

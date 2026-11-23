import { describe, expect, it } from 'vitest';
import { Check, DataError } from '../../data/check';
import { parseRouteStory } from './data';
import { SITES, storyJson } from './fixture';

const parse = (value: unknown): ReturnType<typeof parseRouteStory> =>
  parseRouteStory(value, new Check('story-route.json'), 4, SITES, 420);

function problem(value: unknown): string {
  try {
    parse(value);
  } catch (error) {
    if (error instanceof DataError) return error.problem;
    throw error;
  }
  return 'accepted';
}

type Run = Record<string, unknown>;
const run = (s: Record<string, unknown>): Run =>
  ((s.directions as Record<string, unknown>[])[0]?.strategies as Record<string, Run>)['lowest-delay'] as Run;

describe('story-route.json checks', () => {
  it('read a good file', () => {
    const story = parse(storyJson());
    const r = story.directions[0]?.strategies['lowest-delay'];
    expect(r?.intervals[1]).toEqual({ first_s: 5, last_s: 9, route: [1, 2], gateway: null });
    expect(r?.drops[0]).toEqual({ start_s: 10, length_s: 5, cause: 'ground_link_lost', element: { kind: 'satellite', sat: 2 } });
    expect(r?.summary.changes).toBe(1);
    expect(r?.delay_ms[12]).toBeNull();
  });

  it('refuse a missing field', () => {
    const story = storyJson();
    delete (run(story).summary as Record<string, unknown>).delay_median_ms;
    expect(problem(story)).toBe('directions[0].strategies.lowest-delay.summary.delay_median_ms is missing');
  });

  it('refuse a missing strategy', () => {
    const story = storyJson();
    (story.settings as Record<string, unknown>).strategies = ['lowest-delay', 'longest-lived'];
    expect(problem(story)).toBe('directions[0].strategies.longest-lived is missing');
  });

  it('refuse a wrong type', () => {
    const story = storyJson();
    (run(story).delay_ms as unknown[])[3] = '20.1';
    expect(problem(story)).toBe('directions[0].strategies.lowest-delay.delay_ms[3] should be a number');
  });

  it('refuse a satellite index out of range in a route', () => {
    const story = storyJson();
    (run(story).intervals as unknown[][])[0] = [0, 4, [0, 4], null];
    expect(problem(story)).toBe('directions[0].strategies.lowest-delay.intervals[0][2][1] is 4, but there are only 4 satellites (indices 0 to 3)');
  });

  it('refuse a satellite index out of range in a drop', () => {
    const story = storyJson();
    run(story).drops = [[10, 5, 'ground_link_lost', ['satellite', 7]]];
    expect(problem(story)).toBe('directions[0].strategies.lowest-delay.drops[0][3][1] is 7, but there are only 4 satellites (indices 0 to 3)');
  });

  it('refuse a city that is not a city', () => {
    const story = storyJson();
    (story.directions as Record<string, unknown>[])[0] = { ...(story.directions as Record<string, unknown>[])[0], to: 2 };
    expect(problem(story)).toBe('directions[0].to is site 2, a gateway, not a city');
  });

  it('refuse intervals out of order or past the run', () => {
    const story = storyJson();
    (run(story).intervals as unknown[][])[1] = [3, 9, [1, 2], null];
    expect(problem(story)).toMatch(/^directions\[0\]\.strategies\.lowest-delay\.intervals\[1\] should start at step 5 or later/);
    const late = storyJson();
    (run(late).intervals as unknown[][])[2] = [15, 21, [2, 3], null];
    expect(problem(late)).toMatch(/intervals\[2\] should start at step 10 or later, end at or after its start and before step 21/);
  });

  it('refuse a delay list of the wrong length and overlapping drops', () => {
    const story = storyJson();
    (run(story).delay_ms as unknown[]).pop();
    expect(problem(story)).toBe('directions[0].strategies.lowest-delay.delay_ms should have 21 items but has 20');
    const drops = storyJson();
    run(drops).drops = [[10, 5, 'no_path', null], [12, 1, 'no_path', null]];
    expect(problem(drops)).toMatch(/drops\[1\] should start at 15 s or later/);
  });

  it('refuse a run outside the position window', () => {
    const story = storyJson();
    story.run_start_s = 410;
    expect(problem(story)).toMatch(/outside the position window/);
  });
});

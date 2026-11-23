import { describe, expect, it } from 'vitest';
import { Check, DataError } from '../../data/check';
import { parseResilienceStory } from './data';
import { SITES, storyJson } from './fixture';

const parse = (value: unknown): ReturnType<typeof parseResilienceStory> =>
  parseResilienceStory(value, new Check('story-resilience.json'), 4, SITES, 420, (p) => (p === 'three' ? 3 : 0));

function problem(value: unknown): string {
  try {
    parse(value);
  } catch (error) {
    if (error instanceof DataError) return error.problem;
    throw error;
  }
  return 'accepted';
}

type Obj = Record<string, unknown>;
const step = (s: Obj): Obj => (s.steps as Obj[])[0] as Obj;
const recovery = (s: Obj): Obj => (s.charts as Obj).recovery_summary as Obj;

describe('story-resilience.json checks', () => {
  it('read a good file', () => {
    const story = parse(storyJson());
    expect(story.steps[0]?.routes[1]?.reference.state).toBe('no_route');
    expect(story.steps[0]?.failed_links).toEqual([[0, 1], [1, 2]]);
    expect(story.charts.recovery_summary.rows[3]?.[6]).toBeNull();
  });

  it('refuse a missing field', () => {
    const story = storyJson();
    delete step(story).planned_links_not_failed;
    expect(problem(story)).toBe('steps[0].planned_links_not_failed is missing');
  });

  it('refuse a summary without a column the charts read', () => {
    const story = storyJson();
    const t = recovery(story);
    t.columns = (t.columns as string[]).map((c) => (c === 'reach_reference_mean' ? 'reach_mean' : c));
    expect(problem(story)).toBe('charts.recovery_summary.columns have no column reach_reference_mean');
  });

  it('refuse a wrong type', () => {
    const story = storyJson();
    step(story).t_s = '67';
    expect(problem(story)).toBe('steps[0].t_s should be a number');
    const row = storyJson();
    ((recovery(row).rows as unknown[][])[0] as unknown[])[3] = true;
    expect(problem(row)).toBe('charts.recovery_summary.rows[0][3] should be a number');
  });

  it('refuse a satellite index out of range', () => {
    const story = storyJson();
    step(story).failed_satellites = [4];
    expect(problem(story)).toBe('steps[0].failed_satellites[0] is 4, but there are only 4 satellites (indices 0 to 3)');
    const route = storyJson();
    (((step(route).routes as Obj[])[0] as Obj).reference as Obj).route = [0, 9];
    expect(problem(route)).toBe('steps[0].routes[0].reference.route[1] is 9, but there are only 4 satellites (indices 0 to 3)');
  });

  it('refuse a planned link index out of range', () => {
    const story = storyJson();
    step(story).planned_links_not_failed = [3];
    expect(problem(story)).toBe('steps[0].planned_links_not_failed[0] is 3, but there are only 3 planned links of three (indices 0 to 2)');
  });

  it('refuse an unknown state and a row of the wrong length', () => {
    const story = storyJson();
    (((step(story).routes as Obj[])[0] as Obj).reference as Obj).state = 'lost';
    expect(problem(story)).toMatch(/steps\[0\]\.routes\[0\]\.reference\.state is "lost"/);
    const short = storyJson();
    (recovery(short).rows as unknown[][]).push(['attack']);
    expect(problem(short)).toBe('charts.recovery_summary.rows[4] should have 9 items but has 1');
  });
});

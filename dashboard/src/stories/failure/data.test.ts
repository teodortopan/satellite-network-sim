import { describe, expect, it } from 'vitest';
import { Check, DataError } from '../../data/check';
import { parseFailureStory } from './data';
import { SITES, storyJson } from './fixture';

const parse = (value: unknown): ReturnType<typeof parseFailureStory> =>
  parseFailureStory(value, new Check('story-failure.json'), 4, SITES, 420);

function problem(value: unknown): string {
  try {
    parse(value);
  } catch (error) {
    if (error instanceof DataError) return error.problem;
    throw error;
  }
  return 'accepted';
}

type Story = ReturnType<typeof storyJson>;
const scenario = (s: Story): Record<string, unknown> => (s.scenarios as Record<string, unknown>[])[0] as Record<string, unknown>;

describe('story-failure.json checks', () => {
  it('read a good file', () => {
    const story = parse(storyJson());
    const s = story.scenarios[0];
    expect(s?.lanes.planner[1]?.failed_element).toEqual({ kind: 'satellite', sat: 1 });
    expect(s?.measures.flooding.reports).toBeNull();
    expect(s?.facts[0]?.detectors[1]).toEqual({ kind: 'gateway', index: 2, t_s: 10.05 });
  });

  it('refuse a satellite index out of range in a route', () => {
    const story = storyJson();
    const lanes = scenario(story).lanes as Record<string, unknown[][]>;
    lanes.planner = [[0, 20, 'delivered', null, null, 1, [0, 4], 0, 2]];
    expect(problem(story)).toBe('scenarios[0].lanes.planner[0][6][1] is 4, but there are only 4 satellites (indices 0 to 3)');
  });

  it('refuse a site of the wrong kind', () => {
    const story = storyJson();
    scenario(story).home_pop = 0;
    expect(problem(story)).toBe('scenarios[0].home_pop is site 0, a city, not a pop');
  });

  it('refuse a missing measure', () => {
    const story = storyJson();
    const measures = scenario(story).measures as Record<string, Record<string, unknown>>;
    delete measures.planner?.lost_s;
    expect(problem(story)).toBe('scenarios[0].measures.planner.lost_s is missing');
  });

  it('refuse a lane with a gap', () => {
    const story = storyJson();
    const lanes = scenario(story).lanes as Record<string, unknown[][]>;
    lanes.flooding = [[0, 10, 'delivered', null, null, 1, [0], 0, 2], [11, 20, 'delivered', null, null, 1, [0], 0, 2]];
    expect(problem(story)).toBe('scenarios[0].lanes.flooding[1] should start at 10 s and end after it');
  });

  it('refuse a run outside the position window', () => {
    const story = storyJson();
    scenario(story).run_start_s = 410;
    expect(problem(story)).toMatch(/outside the position window/);
  });

  it('refuse an unknown state', () => {
    const story = storyJson();
    const lanes = scenario(story).lanes as Record<string, unknown[][]>;
    lanes.baseline = [[0, 20, 'lost', null, null, 1, [0], 0, 2]];
    expect(problem(story)).toMatch(/is "lost", which is not one of/);
  });
});

import { describe, expect, it } from 'vitest';
import { Check, DataError } from '../../data/check';
import { parseConstellationStory } from './data';
import { SATELLITES, storyJson } from './fixture';

function problem(value: unknown, satellites = SATELLITES): string {
  try {
    parseConstellationStory(value, new Check('story-constellation.json'), satellites);
  } catch (error) {
    if (error instanceof DataError) return error.problem;
    throw error;
  }
  return 'accepted';
}

const shells = (s: Record<string, unknown>): Record<string, unknown>[] => s.shells as Record<string, unknown>[];

describe('story-constellation.json checks', () => {
  it('read a good file', () => {
    const story = parseConstellationStory(storyJson(), new Check('story-constellation.json'), SATELLITES);
    expect(story.shells[1]?.planes).toBe(1);
    expect(story.patterns[0]?.by_kind.cross_plane).toBe(1);
  });

  it('refuse a missing field', () => {
    const story = storyJson();
    delete shells(story)[0]?.planes;
    expect(problem(story)).toBe('shells[0].planes is missing');
  });

  it('refuse a wrong type', () => {
    const story = storyJson();
    (shells(story)[1] as Record<string, unknown>).height_min_km = '476';
    expect(problem(story)).toBe('shells[1].height_min_km should be a number');
  });

  it('refuse shells that are not those of satellites.json', () => {
    const story = storyJson();
    shells(story).reverse();
    expect(problem(story)).toBe('shells are 97deg, 53deg, but satellites.json has 53deg, 97deg');
  });

  it('refuse a satellite in a plane its shell does not have', () => {
    expect(problem(storyJson(), { ...SATELLITES, plane: [0, 0, 2, 0, 0] })).toBe(
      'shells give shell 53deg 2 planes, but satellite 2 is in plane 2',
    );
  });
});

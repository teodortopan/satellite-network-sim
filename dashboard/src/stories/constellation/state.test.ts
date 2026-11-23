import { describe, expect, it } from 'vitest';
import { chosen, linksOf } from './state';
import { SATELLITES } from './fixture';

// Links: 0-1 (in plane 0 of 53deg), 1-2 (53deg, planes 0 to 1), 3-4 (97deg).
const PAIRS = Uint32Array.from([0, 1, 1, 2, 3, 4]);

describe('what the constellation story shows for a choice', () => {
  it('chooses every satellite, one shell or one plane', () => {
    expect(Array.from(chosen(SATELLITES, null, null))).toEqual([1, 1, 1, 1, 1]);
    expect(Array.from(chosen(SATELLITES, 0, null))).toEqual([1, 1, 1, 0, 0]);
    expect(Array.from(chosen(SATELLITES, 0, 1))).toEqual([0, 0, 1, 0, 0]);
    expect(Array.from(chosen(SATELLITES, 1, 0))).toEqual([0, 0, 0, 1, 1]);
  });

  it('draws the links inside a shell, and for a plane also those that leave it', () => {
    expect(Array.from(linksOf(PAIRS, chosen(SATELLITES, null, null), false))).toEqual([0, 1, 1, 2, 3, 4]);
    expect(Array.from(linksOf(PAIRS, chosen(SATELLITES, 1, null), false))).toEqual([3, 4]);
    expect(Array.from(linksOf(PAIRS, chosen(SATELLITES, 0, 1), true))).toEqual([1, 2]);
    expect(Array.from(linksOf(PAIRS, chosen(SATELLITES, 0, 0), true))).toEqual([0, 1, 1, 2]);
  });
});

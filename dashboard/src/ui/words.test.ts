import { describe, expect, it } from 'vitest';
import { patternShort, patternWords, shellName } from './words';

describe('plain words for project names', () => {
  it('names a shell by its tilt', () => {
    expect(shellName('53deg')).toBe('Tilted 53°');
    expect(shellName('97.6deg')).toBe('Tilted 97.6°');
    expect(shellName('polar')).toBe('Group polar');
  });

  it('says how many lasers each layout gives a satellite', () => {
    expect(patternShort('plus-grid')).toBe('4 lasers each');
    expect(patternShort('other')).toBe('other');
    expect(patternWords('three')).toMatch(/^each satellite has three laser partners.*“three”\)$/);
    expect(patternWords('other')).toBe('the laser layout is “other”');
  });
});

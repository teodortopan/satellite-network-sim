import { describe, expect, it } from 'vitest';
import { formatCount, formatMs, formatSeconds, formatSpeed } from './format';

describe('formatting', () => {
  it('rounds durations for display only', () => {
    expect(formatSeconds(0.09999999999999432)).toBe('100 ms');
    expect(formatSeconds(0.006964435114952039)).toBe('6.96 ms');
    expect(formatSeconds(143.2)).toBe('143.2 s');
    expect(formatSeconds(8)).toBe('8 s');
    expect(formatSeconds(0)).toBe('0 s');
    expect(formatSeconds(null)).toBe('none');
  });

  it('formats delays, counts and speeds', () => {
    expect(formatMs(66.15023583935356)).toBe('66.2 ms');
    expect(formatMs(133.75)).toBe('134 ms');
    expect(formatMs(1.651715676167023)).toBe('1.65 ms');
    expect(formatCount(186816)).toBe('186,816');
    expect(formatSpeed(0.02)).toBe('0.02x');
    expect(formatSpeed(600)).toBe('600x');
  });
});

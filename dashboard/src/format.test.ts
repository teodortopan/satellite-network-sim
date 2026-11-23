import { describe, expect, it } from 'vitest';
import { formatCount, formatMs, formatSeconds, formatShare, formatSpan, formatSpeed } from './format';

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

  it('formats shares as percentages without rounding a share below one up to 100%', () => {
    expect(formatShare(0.944061)).toBe('94.4%');
    expect(formatShare(1)).toBe('100%');
    expect(formatShare(0.001)).toBe('0.1%');
    expect(formatShare(0.99999)).toBe('99.999%');
    expect(formatShare(null)).toBe('none');
  });

  it('writes a run length in words', () => {
    expect(formatSpan(7200)).toBe('2 hours');
    expect(formatSpan(420)).toBe('7 minutes');
    expect(formatSpan(3600)).toBe('1 hour');
    expect(formatSpan(367)).toBe('367 s');
  });
});

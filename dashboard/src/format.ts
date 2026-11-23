// Number formatting for display: rounding and units only (spec 4.4 "Numbers").

const NONE = 'none';

function significant(value: number, digits: number): string {
  return Number(value.toPrecision(digits)).toLocaleString('en-US', {
    maximumFractionDigits: 20,
    useGrouping: false,
  });
}

/** A duration: below one second in milliseconds (3 significant digits), else seconds to 0.1 s. */
export function formatSeconds(s: number | null): string {
  if (s === null) return NONE;
  if (s === 0) return '0 s';
  if (Math.abs(s) < 1) return `${significant(s * 1000, 3)} ms`;
  return `${(Math.round(s * 10) / 10).toLocaleString('en-US', { useGrouping: false })} s`;
}

/** A delay in milliseconds, 3 significant digits (whole milliseconds from 100 ms up). */
export function formatMs(ms: number | null): string {
  if (ms === null) return NONE;
  if (ms === 0) return '0 ms';
  if (Math.abs(ms) >= 100) return `${Math.round(ms)} ms`;
  return `${significant(ms, 3)} ms`;
}

export function formatCount(n: number | null): string {
  if (n === null) return NONE;
  return n.toLocaleString('en-US');
}

/** An instant of the run, in seconds from its start, to the millisecond. */
export function formatInstant(s: number | null): string {
  if (s === null) return NONE;
  return `${s.toFixed(3)} s`;
}

/** The clock: to the millisecond in slow motion, else to a tenth of a second. */
export function formatClock(t: number, fine: boolean): string {
  return `${t.toFixed(fine ? 3 : 1)} s`;
}

export function formatSpeed(speed: number): string {
  return `${significant(speed, 3)}x`;
}

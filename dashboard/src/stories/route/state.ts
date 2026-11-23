// "What is the state at time t" for the route story: lookups in the exported intervals and
// drops. Nothing here computes a simulation value; it only picks one.

import type { Drop, RouteInterval, StrategyRun } from './data';

/** The step shown at story time t: floor(t), and the last step at the window's end. */
export function stepAt(t: number, steps: number): number | null {
  if (!(t >= 0) || t > steps - 1) return null;
  return Math.floor(t);
}

/** The last item whose start is at or before `x` (binary search over sorted starts), or -1. */
function lastStartingBy<T>(items: readonly T[], start: (item: T) => number, x: number): number {
  let lo = 0;
  let hi = items.length - 1;
  let found = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (start(items[mid] as T) <= x) {
      found = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return found;
}

/** The interval holding `step` (both ends included), or null. */
export function intervalAtStep(intervals: readonly RouteInterval[], step: number): RouteInterval | null {
  const i = lastStartingBy(intervals, (iv) => iv.first_s, step);
  const iv = intervals[i];
  return iv !== undefined && step <= iv.last_s ? iv : null;
}

/** The drop holding `step`, over [start_s, start_s + length_s), or null. */
export function dropAtStep(drops: readonly Drop[], step: number): Drop | null {
  const i = lastStartingBy(drops, (d) => d.start_s, step);
  const d = drops[i];
  return d !== undefined && step < d.start_s + d.length_s ? d : null;
}

export type RouteState =
  | { kind: 'route'; step: number; interval: RouteInterval; delay_ms: number | null }
  /** `broken`: the route held up to the drop, drawn broken while the drop lasts (null at the run's start). */
  | { kind: 'drop'; step: number; drop: Drop; broken: RouteInterval | null }
  | { kind: 'none'; step: number | null };

export function stateAt(run: StrategyRun, t: number): RouteState {
  const step = stepAt(t, run.delay_ms.length);
  if (step === null) return { kind: 'none', step };
  const interval = intervalAtStep(run.intervals, step);
  if (interval !== null) return { kind: 'route', step, interval, delay_ms: run.delay_ms[step] ?? null };
  const drop = dropAtStep(run.drops, step);
  if (drop !== null) return { kind: 'drop', step, drop, broken: intervalAtStep(run.intervals, drop.start_s - 1) };
  return { kind: 'none', step };
}

/** The steps where the route changes with no drop between: an interval that starts right
 * after the one before it ends. Their number is the summary's `changes`. */
export function changeSteps(intervals: readonly RouteInterval[]): number[] {
  const out: number[] = [];
  for (let i = 1; i < intervals.length; i++) {
    const iv = intervals[i] as RouteInterval;
    if (iv.first_s === (intervals[i - 1] as RouteInterval).last_s + 1) out.push(iv.first_s);
  }
  return out;
}

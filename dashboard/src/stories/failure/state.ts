// "What is the state at time t" for the failure story: lookups in the exported intervals,
// facts and plans. Nothing here computes a simulation value; it only picks one.

import type { PathNode } from '../../globe/paths';
import type { Element, Fact, Interval } from './data';

/**
 * The interval holding story time t: [start_s, end_s), and the last one also at its end, so the
 * clock's last instant still has a state. Null outside the intervals.
 */
export function intervalAt(intervals: readonly Interval[], t: number): Interval | null {
  let lo = 0;
  let hi = intervals.length - 1;
  let found = -1;
  // The last interval whose start is at or before t.
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if ((intervals[mid] as Interval).start_s <= t) {
      found = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  if (found < 0) return null;
  const iv = intervals[found] as Interval;
  const last = found === intervals.length - 1;
  return t < iv.end_s || (last && t === iv.end_s) ? iv : null;
}

/** The earliest instant the planner knew any fact of this run, or null if it never did. */
export function plannerFirstKnown(facts: readonly Fact[]): number | null {
  let first: number | null = null;
  for (const f of facts) {
    if (f.old || f.planner_known_s === null) continue;
    if (first === null || f.planner_known_s < first) first = f.planner_known_s;
  }
  return first;
}

/** The fact the planner knew first (the one whose reports the lanes show). */
export function firstKnownFact(facts: readonly Fact[]): Fact | null {
  const first = plannerFirstKnown(facts);
  return facts.find((f) => !f.old && f.planner_known_s === first) ?? null;
}

/** When the new plan takes effect: the first `replaced` plan at or after `known_s`. */
export function newPlanAt(plans: readonly [number, string][], known_s: number | null): number | null {
  if (known_s === null) return null;
  const plan = plans.find(([t, what]) => what === 'replaced' && t >= known_s);
  return plan === undefined ? null : plan[0];
}

/** The earliest detection among a fact's detectors, or null if nobody detected it. */
export function firstDetection(fact: Fact): number | null {
  let first: number | null = null;
  for (const d of fact.detectors) if (first === null || d.t_s < first) first = d.t_s;
  return first;
}

/** For each satellite, the earliest instant it detected any fact (`never` if it never did). */
export function satelliteDetections(facts: readonly Fact[], satellites: number, never: number): Float32Array {
  const out = new Float32Array(satellites).fill(never);
  for (const f of facts) {
    for (const d of f.detectors) {
      if (d.kind === 'satellite' && d.t_s < (out[d.index] as number)) out[d.index] = d.t_s;
    }
  }
  return out;
}

/** What each actor knows of a fact at time t. */
export interface FactView {
  happened: boolean;
  detected: boolean;
  plannerKnows: boolean;
  floodDone: boolean;
}

export function factAt(fact: Fact, t: number): FactView {
  const detected = firstDetection(fact);
  return {
    happened: fact.old || t >= fact.t_s,
    detected: detected !== null && t >= detected,
    plannerKnows: fact.planner_known_s !== null && t >= fact.planner_known_s,
    floodDone: fact.flood_last_s !== null && t >= fact.flood_last_s,
  };
}

export interface DrawnPath {
  /** Up through the satellites: from the sending city on. */
  space: PathNode[];
  /** Over the ground, by fibre (sites), drawn only when traffic is delivered. */
  ground: number[];
  /** Where traffic is lost (black hole), drawn at the end of `space`. */
  lost: PathNode | null;
}

/**
 * The route of an interval as it is drawn: the whole path when delivered (in the realistic
 * model up to the gateway, then over fibre to the home PoP and the receiving city), up to the
 * failed element for a black hole, the walk for a loop, nothing without a route.
 */
export function drawnPath(iv: Interval, from: number, to: number, homePop: number): DrawnPath {
  const sats: PathNode[] = iv.route.map((sat) => ({ kind: 'sat', sat }));
  const start: PathNode = { kind: 'site', site: from };
  if (iv.state === 'no_route') return { space: [], ground: [], lost: null };
  if (iv.state === 'loop') return { space: [start, ...sats], ground: [], lost: null };
  if (iv.state === 'delivered') {
    if (iv.gateway === null) return { space: [start, ...sats, { kind: 'site', site: to }], ground: [], lost: null };
    return { space: [start, ...sats, { kind: 'site', site: iv.gateway }], ground: [iv.gateway, homePop, to], lost: null };
  }
  // A failed satellite (or a failed link's end) on the held route: traffic gets that far.
  const cut = routeCut(iv.route, iv.failed_element);
  if (cut >= 0) {
    const kept = sats.slice(0, cut + 1);
    return { space: [start, ...kept], ground: [], lost: kept[kept.length - 1] ?? start };
  }
  // Otherwise the route stops before the dead element (flooding's walk, a dead gateway).
  const lost = lostNode(iv.failed_element);
  if (lost === null) return { space: [start, ...sats], ground: [], lost: sats[sats.length - 1] ?? start };
  return { space: [start, ...sats, lost], ground: [], lost };
}

function lostNode(e: Element | null): PathNode | null {
  if (e === null) return null;
  switch (e.kind) {
    case 'satellite':
      return { kind: 'sat', sat: e.sat };
    case 'link':
      return null;
    case 'gateway':
    case 'city':
      return { kind: 'site', site: e.site };
  }
}

/** Index in the route of the failed satellite, or of a failed link's first end met. */
function routeCut(route: readonly number[], e: Element | null): number {
  if (e === null) return -1;
  if (e.kind === 'satellite') return route.indexOf(e.sat);
  if (e.kind === 'link') return route.findIndex((s) => s === e.a || s === e.b);
  return -1;
}

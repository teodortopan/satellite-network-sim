// What the "Try it" page works out besides drawing, all without the DOM: the address that
// keeps a choice, the name of a point picked on the globe, whether two places make sense as a
// pair, the run's time window, and what the headline says at the clock's time. The headline
// takes every number from the engine's run (a Direction from engine/from-csv.ts) and only
// looks up the state at a time, as the route view's readouts do.

import { formatCount, formatMs, formatSpan } from '../../format';
import { groundDistanceKm } from '../../globe/coords';
import type { Place } from '../../data/places';
import type { EngineManifest } from '../../engine/settings';
import { utcSeconds } from '../../engine/run';
import type { Direction, Drop, StrategyRun } from '../route/data';
import { stateAt } from '../route/state';
import { CAUSE_TITLES } from '../route/view';

/** A place of a run: its short name and where it is. */
export interface TripPlace {
  name: string;
  lat_deg: number;
  lon_deg: number;
}

export interface Trip {
  from: TripPlace;
  to: TripPlace;
}

/** The longest a run on this page lasts. */
export const MAX_RUN_S = 600;
/** A picked point takes the name of a listed place this close. */
export const NAME_WITHIN_KM = 30;
/** Two places closer than this are too close to be worth a route through space. */
export const TOO_CLOSE_KM = 20;
/** The rule the headline follows. */
export const HEADLINE_RULE = 'lowest-delay';
const MAX_NAME = 80;

// ---------------------------------------------------------------------------------------
// The address: #explore?from=Lagos,6.4452,3.3896&to=Sydney,-33.8714,151.2125
// ---------------------------------------------------------------------------------------

const coordinate = (v: number): string => String(Math.round(v * 1e4) / 1e4);

function encodePlace(p: TripPlace): string {
  return `${encodeURIComponent(p.name)},${coordinate(p.lat_deg)},${coordinate(p.lon_deg)}`;
}

function decodePlace(text: string | undefined): TripPlace | null {
  if (text === undefined) return null;
  const parts = text.split(',');
  if (parts.length !== 3) return null;
  let name: string;
  try {
    name = decodeURIComponent(parts[0] as string).trim();
  } catch {
    return null;
  }
  const number = (s: string): number => (/^-?\d+(\.\d+)?$/.test(s) ? Number(s) : Number.NaN);
  const lat = number(parts[1] as string);
  const lon = number(parts[2] as string);
  if (name === '' || name.length > MAX_NAME || !(Math.abs(lat) <= 90) || !(Math.abs(lon) <= 180)) return null;
  return { name, lat_deg: lat, lon_deg: lon };
}

/** The address of a choice, for the page with id `story`. */
export function encodeTrip(story: string, trip: Trip): string {
  return `#${story}?from=${encodePlace(trip.from)}&to=${encodePlace(trip.to)}`;
}

/** The choice an address holds, or null if it holds none (or a broken one). */
export function decodeTrip(hash: string): Trip | null {
  const q = hash.indexOf('?');
  if (q < 0) return null;
  const fields = new Map<string, string>();
  for (const part of hash.slice(q + 1).split('&')) {
    const eq = part.indexOf('=');
    if (eq > 0) fields.set(part.slice(0, eq), part.slice(eq + 1));
  }
  const from = decodePlace(fields.get('from'));
  const to = decodePlace(fields.get('to'));
  return from === null || to === null ? null : { from, to };
}

// ---------------------------------------------------------------------------------------
// Places.
// ---------------------------------------------------------------------------------------

/** "Point at 52.1° N, 4.3° E". */
export function pointName(lat_deg: number, lon_deg: number): string {
  const one = (v: number): string => (Math.round(Math.abs(v) * 10) / 10).toFixed(1);
  const ns = Math.round(lat_deg * 10) === 0 ? '' : lat_deg > 0 ? ' N' : ' S';
  const ew = Math.round(lon_deg * 10) === 0 || Math.round(Math.abs(lon_deg) * 10) === 1800 ? '' : lon_deg > 0 ? ' E' : ' W';
  return `Point at ${one(lat_deg)}°${ns}, ${one(lon_deg)}°${ew}`;
}

/** The listed place nearest to a point, if one is within `within_km`; the largest on a tie. */
export function nearestPlace(places: readonly Place[], at: { lat_deg: number; lon_deg: number }, within_km = NAME_WITHIN_KM): Place | null {
  let best: Place | null = null;
  let bestKm = within_km;
  for (const p of places) {
    // A cheap box first: a degree of latitude is about 111 km.
    if (Math.abs(p.lat_deg - at.lat_deg) > within_km / 100) continue;
    const km = groundDistanceKm(p, at);
    if (km < bestKm || (best !== null && km === bestKm && p.population > best.population)) {
      best = p;
      bestKm = km;
    }
  }
  return best;
}

export type PairProblem = { kind: 'same' } | { kind: 'close'; km: number };

/** Why two places make no pair worth a route, or null. */
export function pairProblem(trip: Trip): PairProblem | null {
  const km = groundDistanceKm(trip.from, trip.to);
  if (km < 1) return { kind: 'same' };
  if (km < TOO_CLOSE_KM) return { kind: 'close', km };
  return null;
}

export function pairProblemText(trip: Trip, problem: PairProblem): string {
  if (problem.kind === 'same') {
    return `From and To are the same place (${trip.from.name}). Choose two different places.`;
  }
  return (
    `${trip.from.name} and ${trip.to.name} are only ${Math.max(1, Math.round(problem.km))} km apart: a message would just go up to ` +
    `one satellite and straight back down. Choose two places farther apart, such as two cities in different countries.`
  );
}

// ---------------------------------------------------------------------------------------
// The run.
// ---------------------------------------------------------------------------------------

/** The run of this page: from the start of the engine's window, at most MAX_RUN_S long. */
export function runWindow(engine: EngineManifest): { start_utc: string; duration_s: number } {
  const step = engine.settings.step_s;
  const duration = Math.floor(Math.min(MAX_RUN_S, engine.window.duration_s) / step) * step;
  return { start_utc: engine.window.start_utc, duration_s: duration };
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];

/** "7 minutes of 18 November 2026, from 21:30:30 to 21:37:30 UTC (world time)". */
export function windowWords(start_utc: string, duration_s: number): string {
  const start = utcSeconds(start_utc);
  if (start === null) return `${formatSpan(duration_s)} from ${start_utc} UTC`;
  const a = new Date(start * 1000);
  const b = new Date((start + duration_s) * 1000);
  const day = `${a.getUTCDate()} ${MONTHS[a.getUTCMonth()] ?? ''} ${a.getUTCFullYear()}`;
  const time = (d: Date): string => d.toISOString().slice(11, 19);
  return `${formatSpan(duration_s)} of ${day}, from ${time(a)} to ${time(b)} UTC (world time)`;
}

// ---------------------------------------------------------------------------------------
// The headline.
// ---------------------------------------------------------------------------------------

export type Headline =
  /** A route at the clock's time: the rule's delay and the satellites on it. */
  | { kind: 'route'; text: string; delay_ms: number; satellites: number }
  /** No route at the clock's time, with the engine's cause. */
  | { kind: 'drop'; text: string; cause: string }
  /** No route at any time of the run, with the engine's main cause. */
  | { kind: 'never'; text: string; cause: string }
  | { kind: 'none'; text: string };

function causeWords(cause: string): string {
  return CAUSE_TITLES[cause] ?? cause.replaceAll('_', ' ');
}

/** The cause the engine gave for the most seconds without a route, and a drop of it. */
function mainCause(run: StrategyRun): { cause: string; drop: Drop | null } {
  let cause = '';
  let most = -1;
  for (const [c, s] of Object.entries(run.summary.no_route_s)) {
    if (s > most) {
      most = s;
      cause = c;
    }
  }
  if (cause === '') cause = run.drops[0]?.cause ?? 'no_path';
  return { cause, drop: run.drops.find((d) => d.cause === cause) ?? null };
}

/**
 * What the headline says at story time `t` for one direction, under HEADLINE_RULE. `names`
 * are the two places' names; `direction.from` and `direction.to` are the numbers its drop
 * elements of kind city use for them.
 */
export function headlineAt(direction: Direction, names: { from: string; to: string }, t: number): Headline {
  const run = direction.strategies[HEADLINE_RULE];
  const trip = `${names.from} → ${names.to}`;
  if (run === undefined) return { kind: 'none', text: trip };
  if (run.summary.ok_steps === 0) {
    const { cause, drop } = mainCause(run);
    const e = drop?.element;
    const where = e?.kind === 'city' ? (e.site === direction.from ? names.from : e.site === direction.to ? names.to : null) : null;
    const text =
      cause === 'no_coverage' && where !== null
        ? `No Starlink satellite of this snapshot passes within view of ${where} during these minutes, so no message can get through.`
        : `No connection between ${names.from} and ${names.to} at any moment of these minutes: ${causeWords(cause)}.`;
    return { kind: 'never', text, cause };
  }
  const state = stateAt(run, t);
  if (state.kind === 'route' && state.delay_ms !== null) {
    const n = state.interval.route.length;
    return {
      kind: 'route',
      text: `${trip}: about ${formatMs(state.delay_ms)} one way, through ${formatCount(n)} ${n === 1 ? 'satellite' : 'satellites'}`,
      delay_ms: state.delay_ms,
      satellites: n,
    };
  }
  if (state.kind === 'drop') {
    return { kind: 'drop', text: `${trip}: no connection at this moment, because ${causeWords(state.drop.cause)}`, cause: state.drop.cause };
  }
  return { kind: 'none', text: trip };
}

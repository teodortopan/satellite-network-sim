import { describe, expect, it } from 'vitest';
import type { RouteState } from './state';
import { stateText, strategyTitle } from './view';

const name = (): string => 'STARLINK-1';

describe('the readout of a rule, in plain words', () => {
  it('gives the travel time and the satellites of a route', () => {
    const state: RouteState = { kind: 'route', step: 3, interval: { first_s: 0, last_s: 9, route: [1, 2, 3, 4], gateway: null }, delay_ms: 20.64 };
    expect(stateText(state, name)).toBe('20.6 ms one way, through 4 satellites');
  });

  it('says one satellite, not one satellites', () => {
    const state: RouteState = { kind: 'route', step: 0, interval: { first_s: 0, last_s: 0, route: [1], gateway: null }, delay_ms: 5 };
    expect(stateText(state, name)).toBe('5 ms one way, through 1 satellite');
  });

  it('says how long there is no connection, from when, why and what broke', () => {
    const state: RouteState = {
      kind: 'drop',
      step: 20,
      drop: { start_s: 16, length_s: 14, cause: 'serving_left_view', element: { kind: 'satellite', sat: 1 } },
      broken: null,
    };
    expect(stateText(state, name)).toBe('No connection for 14 s (from 16 s): the satellite above a place moved out of view (STARLINK-1)');
  });

  it('keeps a cause it has no words for, readably', () => {
    const state: RouteState = { kind: 'drop', step: 0, drop: { start_s: 0, length_s: 1, cause: 'new_cause', element: null }, broken: null };
    expect(stateText(state, () => '')).toBe('No connection for 1 s (from 0 s): new cause');
  });

  it('names an unknown rule by its own name', () => {
    expect(strategyTitle('lowest-delay')).toBe('Always the fastest');
    expect(strategyTitle('other')).toBe('other');
  });
});

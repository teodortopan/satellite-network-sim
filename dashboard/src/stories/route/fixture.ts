// A hand-made route story for the unit tests: 4 satellites, 3 sites, a run of 20 s (21 steps)
// with one strategy: a route over steps 0 to 4, a change at 5, a drop over 10 to 14, a route
// again from 15 to the end.

import type { Site } from '../../data/common';

export const SITES: Site[] = [
  { name: 'From', kind: 'city', lat_deg: 40.7, lon_deg: -74 },
  { name: 'To', kind: 'city', lat_deg: 51.5, lon_deg: -0.1 },
  { name: 'Gateway', kind: 'gateway', lat_deg: 50, lon_deg: 8 },
];

export function storyJson(): Record<string, unknown> {
  const delay = Array.from({ length: 21 }, (_, i) => (i >= 10 && i < 15 ? null : 20 + i / 10));
  return {
    sources: { routes: 'routing.routes.csv', drops: 'routing.drops.csv', summary: 'routing.summary.txt' },
    settings: {
      model: 'best-case',
      pattern: 'three',
      replan_s: 15,
      planner: 'start',
      backup: 'on',
      step_s: 1,
      start_utc: '2026-11-18T21:30:30',
      duration_s: 20,
      strategies: ['lowest-delay'],
    },
    run_start_s: 0,
    directions: [
      {
        from: 0,
        to: 1,
        strategies: {
          'lowest-delay': {
            intervals: [
              [0, 4, [0, 1], null],
              [5, 9, [1, 2], null],
              [15, 20, [2, 3], null],
            ],
            delay_ms: delay,
            drops: [[10, 5, 'ground_link_lost', ['satellite', 2]]],
            summary: {
              ok_steps: 16,
              steps: 21,
              delay_median_ms: 20.9,
              bad_changes: 1,
              increases: 0,
              breaks: 1,
              recoveries: 1,
              drop_events: 1,
              no_route_s: { ground_link_lost: 5 },
              changes: 1,
              no_route_total_s: 5,
            },
          },
        },
      },
    ],
  };
}

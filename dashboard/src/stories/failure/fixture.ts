// A hand-made failure story for the unit tests: 4 satellites, 3 sites, a run of 20 s with a
// failure at t = 10.

import type { Site } from '../../data/common';

export const SITES: Site[] = [
  { name: 'From', kind: 'city', lat_deg: 64, lon_deg: -22 },
  { name: 'To', kind: 'city', lat_deg: 43, lon_deg: 132 },
  { name: 'Gateway', kind: 'gateway', lat_deg: 51.5, lon_deg: -0.1 },
  { name: 'pop', kind: 'pop', lat_deg: 51.5, lon_deg: -0.1 },
];

const delivered = (from: number, to: number, route: number[]): unknown[] => [from, to, 'delivered', null, null, 66.5, route, route[0], 2];

export function storyJson(): Record<string, unknown> {
  const measures = {
    failure_s: 10,
    black_hole_s: 2,
    loop_s: 0,
    lost_s: 2,
    restore_s: 2,
    settle_s: 0,
    extra_min_ms: 0,
    extra_median_ms: 0,
    extra_max_ms: 1.5,
    extra_settled_ms: null,
    reports: 1,
    messages: 2,
  };
  const lane = [
    delivered(0, 10, [0, 1]),
    [10, 12, 'black_hole', 'failed_satellite', ['satellite', 1], null, [0, 1], 0, 2],
    delivered(12, 20, [0, 2, 3]),
  ];
  return {
    settings: { pattern: 'three' },
    from: 0,
    to: 1,
    instant: 1,
    instant_utc: '2026-11-18T21:31:37',
    scenarios: [
      {
        scenario: 'route-satellite',
        sources: { measures: 'm.csv', events: 'e.csv' },
        start_utc: '2026-11-18T21:30:30',
        duration_s: 20,
        run_start_s: 0,
        failure_s: 10,
        failed: { satellites: [1], links: [], gateways: [] },
        home_pop: 3,
        lanes: {
          baseline: [delivered(0, 20, [0, 1])],
          reference: lane,
          planner: lane,
          'local-repair': lane,
          flooding: lane,
        },
        plans: { baseline: [[0, 'replaced']], reference: [], planner: [[0, 'replaced'], [15, 'replaced']], 'local-repair': [], flooding: [] },
        measures: { planner: measures, 'local-repair': measures, flooding: { ...measures, reports: null } },
        facts: [
          {
            fact: 0,
            subject: ['satellite', 1],
            state: 'down',
            t_s: 10,
            old: false,
            detectors: [['satellite', 0, 10.1], ['gateway', 2, 10.05]],
            planner_known_s: 10.05,
            reports: 1,
            report_hops: 0,
            flood_nodes: 3,
            flood_last_s: 10.2,
            messages: 2,
          },
        ],
        flood: { satellite_first_learn_s: [10.1, null, 10.15, 10.2], site_first_learn_s: [null, null, 10.05, null] },
        slow_motion: [{ from_s: 10, to_s: 11, speed: 0.02 }],
        pause_before_failure_real_s: 2,
        context: { source: 'docs/phase-4-data/measures.csv', this_run: 'r', rows: [{ run: 'r', model: 'realistic', from: 'A', to: 'B', instant: 1, reaction: 'planner', lost_s: 2 }] },
      },
    ],
  };
}

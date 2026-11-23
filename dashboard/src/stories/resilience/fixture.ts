// A hand-made resilience story for the unit tests: 4 satellites, 3 cities, links-three with 3
// links, one step with satellite 1 failed.

import type { Site } from '../../data/common';

export const SITES: Site[] = [
  { name: 'A', kind: 'city', lat_deg: 40.7, lon_deg: -74 },
  { name: 'B', kind: 'city', lat_deg: 51.5, lon_deg: -0.1 },
  { name: 'C', kind: 'city', lat_deg: 35.7, lon_deg: 139.7 },
  { name: 'G', kind: 'gateway', lat_deg: 50, lon_deg: 8 },
];

/** links-three.json's links as pairs: 0-1, 1-2, 2-3. */
export const THREE = Uint32Array.from([0, 1, 1, 2, 2, 3]);

const delivered = (route: number[], ms: number): Record<string, unknown> => ({ state: 'delivered', drop_cause: null, one_way_ms: ms, route });

export function storyJson(): Record<string, unknown> {
  return {
    charts: {
      recovery_summary: {
        source: 'docs/phase-4-data/recovery_summary.csv',
        columns: ['group', 'scenario', 'pattern', 'share', 'repoint', 'runs', 'reach_reference_mean', 'reach_reference_p2_5', 'reach_reference_p97_5'],
        rows: [
          ['random-satellites', 'random-satellites', 'three', 0.01, 'off', 30, 1.0, 1.0, 1.0],
          ['random-satellites', 'random-satellites', 'three', 0.3, 'off', 30, 0.944061, 0.876116, 0.994939],
          ['attack', 'attack', 'three', 0.3, 'off', 10, 0.76, 0.567, 0.888],
          ['pattern', 'random-satellites', 'six', 0.3, 'off', 30, null, null, null],
        ],
      },
      percolation: {
        source: 'docs/phase-4-data/percolation.csv',
        columns: ['links', 'kind', 'failed_share', 'seeds', 'mean_largest_piece_share', 'min_largest_piece_share', 'max_largest_piece_share', 'threshold_failed_share'],
        rows: [['three', 'satellites', 0.05, 20, 0.999967, 0.99, 1, 0.303]],
      },
    },
    steps: [
      {
        name: 'random-satellites.share-0.01.seed-0',
        sources: { routes: 'r.csv', network: 'n.csv' },
        scenario: 'random-satellites',
        share: 0.01,
        seed: 0,
        instant: 1,
        utc: '2026-11-18T21:31:37',
        t_s: 67,
        pattern: 'three',
        repoint: 'off',
        failed_satellites: [1],
        failed_links: [[0, 1], [1, 2]],
        planned_links_not_failed: [2],
        routes: [
          { from: 0, to: 1, baseline: delivered([0, 1], 21), reference: delivered([0, 2], 23.5) },
          { from: 0, to: 2, baseline: delivered([1], 30), reference: { state: 'no_route', drop_cause: 'no_path', one_way_ms: null, route: [] } },
        ],
        network: [{ scope: 'all', working_satellites: 3, pieces: 2, largest_piece: 2, largest_piece_share: 0.6666666666666666 }],
      },
    ],
  };
}

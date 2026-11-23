// A hand-made constellation story for the unit tests: 2 shells, 5 satellites.

import type { Satellites } from '../../data/common';

export const SATELLITES: Satellites = {
  shells: ['53deg', '97deg'],
  norad: [1, 2, 3, 4, 5],
  name: ['A', 'B', 'C', 'D', 'E'],
  shell: [0, 0, 0, 1, 1],
  plane: [0, 0, 1, 0, 0],
};

const shell = (name: string, planes: number): Record<string, unknown> => ({
  shell: name,
  inclination_deg: 53.16,
  inclination_tolerance_deg: 0.3,
  height_min_km: 467.5,
  height_max_km: 471.0,
  plane_gap_deg: 0.4,
  min_plane_size: 10,
  satellites_in_bands: 4,
  satellites_in_planes: 3,
  planes,
  plane_size_min: 1,
  plane_size_median: 1.5,
  plane_size_max: 2,
});

export function storyJson(): Record<string, unknown> {
  return {
    sources: { shells: 'constellation.summary.txt', patterns: ['topology-three.summary.txt'] },
    plan_time_utc: '2026-11-18T21:15:00',
    shells: [shell('53deg', 2), shell('97deg', 1)],
    patterns: [{ pattern: 'three', links: 3, by_kind: { in_plane: 2, cross_plane: 1, second_cross_plane: 0 }, file: 'links-three.json' }],
  };
}

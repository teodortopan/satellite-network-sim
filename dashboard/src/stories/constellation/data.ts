// story-constellation.json (dashboard/data-format.md), read and checked against
// satellites.json: the same shells in the same order, and every satellite's plane among its
// shell's planes.

import type { Check, JsonObject } from '../../data/check';
import type { Satellites } from '../../data/common';

export interface ShellRow {
  shell: string;
  inclination_deg: number;
  inclination_tolerance_deg: number;
  height_min_km: number;
  height_max_km: number;
  plane_gap_deg: number;
  min_plane_size: number;
  satellites_in_bands: number;
  satellites_in_planes: number;
  planes: number;
  plane_size_min: number;
  plane_size_median: number;
  plane_size_max: number;
}

export interface PatternRow {
  pattern: string;
  links: number;
  by_kind: { in_plane: number; cross_plane: number; second_cross_plane: number };
  file: string;
}

export interface ConstellationStory {
  sources: { shells: string; patterns: string[] };
  plan_time_utc: string;
  shells: ShellRow[];
  patterns: PatternRow[];
}

const NUMBERS = ['inclination_deg', 'inclination_tolerance_deg', 'height_min_km', 'height_max_km', 'plane_gap_deg', 'plane_size_median'] as const;
const COUNTS = ['min_plane_size', 'satellites_in_bands', 'satellites_in_planes', 'planes', 'plane_size_min', 'plane_size_max'] as const;
const KINDS = ['in_plane', 'cross_plane', 'second_cross_plane'] as const;

export function parseConstellationStory(value: unknown, c: Check, satellites: Satellites): ConstellationStory {
  const root = c.object(value, '');
  const get = (o: JsonObject, key: string, path: string): unknown => c.field(o, key, path);

  const shells = c.list(get(root, 'shells', ''), 'shells', (v, p) => {
    const o = c.object(v, p);
    const row = { shell: c.string(get(o, 'shell', p), `${p}.shell`) } as ShellRow;
    for (const key of NUMBERS) row[key] = c.number(get(o, key, p), `${p}.${key}`);
    for (const key of COUNTS) row[key] = c.integer(get(o, key, p), `${p}.${key}`);
    return row;
  });
  if (shells.map((s) => s.shell).join(',') !== satellites.shells.join(',')) {
    c.fail('shells', `are ${shells.map((s) => s.shell).join(', ')}, but satellites.json has ${satellites.shells.join(', ')}`);
  }
  satellites.plane.forEach((plane, i) => {
    const shell = shells[satellites.shell[i] as number] as ShellRow;
    if (plane < 0 || plane >= shell.planes) {
      c.fail('shells', `give shell ${shell.shell} ${shell.planes} planes, but satellite ${i} is in plane ${plane}`);
    }
  });

  const patterns = c.list(get(root, 'patterns', ''), 'patterns', (v, p) => {
    const o = c.object(v, p);
    const k = c.object(get(o, 'by_kind', p), `${p}.by_kind`);
    const by_kind = {} as PatternRow['by_kind'];
    for (const key of KINDS) by_kind[key] = c.integer(get(k, key, `${p}.by_kind`), `${p}.by_kind.${key}`);
    return {
      pattern: c.string(get(o, 'pattern', p), `${p}.pattern`),
      links: c.integer(get(o, 'links', p), `${p}.links`),
      by_kind,
      file: c.string(get(o, 'file', p), `${p}.file`),
    };
  });
  if (patterns.length === 0) c.fail('patterns', 'is empty');

  const so = c.object(get(root, 'sources', ''), 'sources');
  return {
    sources: {
      shells: c.string(get(so, 'shells', 'sources'), 'sources.shells'),
      patterns: c.list(get(so, 'patterns', 'sources'), 'sources.patterns', (v, p) => c.string(v, p)),
    },
    plan_time_utc: c.string(get(root, 'plan_time_utc', ''), 'plan_time_utc'),
    shells,
    patterns,
  };
}

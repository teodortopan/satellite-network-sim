// The files every story uses: satellites, sites, laser links and positions
// (dashboard/data-format.md).

import type { Check } from './check';
import type { DataSource } from './loader';

export interface Satellites {
  shells: string[];
  norad: number[];
  name: string[];
  shell: number[];
  plane: number[];
}

export type SiteKind = 'city' | 'gateway' | 'pop';

export interface Site {
  name: string;
  kind: SiteKind;
  lat_deg: number;
  lon_deg: number;
}

export interface Links {
  pattern: string;
  /** Satellite index pairs, two per link. */
  pairs: Uint32Array;
}

export interface CommonData {
  source: DataSource;
  satellites: Satellites;
  sites: Site[];
  /** Every sample, satellite after satellite, x y z in ECEF km: one array for the window. */
  positions: Float32Array;
}

export function parseSatellites(value: unknown, c: Check, count: number): Satellites {
  const root = c.object(value, '');
  const shells = c.list(c.field(root, 'shells', ''), 'shells', (v, p) => c.string(v, p));
  const column = <T>(key: string, read: (v: unknown, p: string) => T): T[] => {
    const items = c.array(c.field(root, key, ''), key, count);
    return items.map((v, i) => read(v, `${key}[${i}]`));
  };
  return {
    shells,
    norad: column('norad_cat_id', (v, p) => c.integer(v, p)),
    name: column('name', (v, p) => c.string(v, p)),
    shell: column('shell', (v, p) => c.index(v, shells.length, p, 'shells')),
    plane: column('plane', (v, p) => c.integer(v, p)),
  };
}

export function parseSites(value: unknown, c: Check): Site[] {
  const root = c.object(value, '');
  return c.list(c.field(root, 'sites', ''), 'sites', (item, p) => {
    const s = c.object(item, p);
    const lat = c.number(c.field(s, 'lat_deg', p), `${p}.lat_deg`);
    const lon = c.number(c.field(s, 'lon_deg', p), `${p}.lon_deg`);
    if (Math.abs(lat) > 90 || Math.abs(lon) > 180) c.fail(p, 'has a latitude or longitude out of range');
    return {
      name: c.string(c.field(s, 'name', p), `${p}.name`),
      kind: c.oneOf(c.field(s, 'kind', p), ['city', 'gateway', 'pop'] as const, `${p}.kind`),
      lat_deg: lat,
      lon_deg: lon,
    };
  });
}

export function parseLinks(value: unknown, c: Check, satellites: number): Links {
  const root = c.object(value, '');
  const items = c.array(c.field(root, 'links', ''), 'links');
  const pairs = new Uint32Array(items.length * 2);
  items.forEach((item, i) => {
    const pair = c.array(item, `links[${i}]`, 2);
    pairs[2 * i] = c.index(pair[0], satellites, `links[${i}][0]`, 'satellites');
    pairs[2 * i + 1] = c.index(pair[1], satellites, `links[${i}][1]`, 'satellites');
  });
  return { pattern: c.string(c.field(root, 'pattern', ''), 'pattern'), pairs };
}

/** All position files joined into one array, read as little-endian whatever the machine. */
export async function loadPositions(source: DataSource): Promise<Float32Array> {
  const { manifest } = source;
  const floatsPerSample = manifest.satellites * 3;
  const out = new Float32Array(manifest.settings.samples * floatsPerSample);
  const buffers = await Promise.all(manifest.positions.map((p) => source.bytes(p.path)));
  manifest.positions.forEach((p, k) => {
    const view = new DataView(buffers[k] as ArrayBuffer);
    const offset = p.first_sample * floatsPerSample;
    const count = p.samples * floatsPerSample;
    for (let i = 0; i < count; i++) out[offset + i] = view.getFloat32(4 * i, true);
  });
  return out;
}

export async function loadCommon(source: DataSource): Promise<CommonData> {
  const count = source.manifest.satellites;
  const [satellites, sites, positions] = await Promise.all([
    source.json('satellites.json', (v, c) => parseSatellites(v, c, count)),
    source.json('sites.json', parseSites),
    loadPositions(source),
  ]);
  return { source, satellites, sites, positions };
}

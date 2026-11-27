// places.json (dashboard/data-format.md): the places a visitor can search by name, from
// Natural Earth's populated places, largest first. Every row is checked; one bad row refuses
// the file.

import type { Check } from './check';

export interface Place {
  name: string;
  /** The state, province or similar; may be empty. */
  region: string;
  country: string;
  lat_deg: number;
  lon_deg: number;
  population: number;
}

export const PLACE_FIELDS = ['name', 'region', 'country', 'lat_deg', 'lon_deg', 'population'] as const;

export function parsePlaces(value: unknown, c: Check): Place[] {
  const root = c.object(value, '');
  const fields = c.list(c.field(root, 'fields', ''), 'fields', (v, p) => c.string(v, p));
  if (fields.length !== PLACE_FIELDS.length || fields.some((f, i) => f !== PLACE_FIELDS[i])) {
    c.fail('fields', `should be ${PLACE_FIELDS.join(', ')}`);
  }
  const places = c.list(c.field(root, 'rows', ''), 'rows', (item, p) => {
    const row = c.array(item, p, PLACE_FIELDS.length);
    const name = c.string(row[0], `${p}[0]`);
    if (name.trim() === '') c.fail(`${p}[0]`, 'is an empty name');
    const lat = c.number(row[3], `${p}[3]`);
    const lon = c.number(row[4], `${p}[4]`);
    if (Math.abs(lat) > 90) c.fail(`${p}[3]`, `is ${lat}, not a latitude`);
    if (Math.abs(lon) > 180) c.fail(`${p}[4]`, `is ${lon}, not a longitude`);
    const population = c.integer(row[5], `${p}[5]`);
    if (population < 0) c.fail(`${p}[5]`, 'should not be negative');
    return {
      name,
      region: c.string(row[1], `${p}[1]`),
      country: c.string(row[2], `${p}[2]`),
      lat_deg: lat,
      lon_deg: lon,
      population,
    };
  });
  if (places.length === 0) c.fail('rows', 'is empty');
  return places;
}

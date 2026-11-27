import { describe, expect, it } from 'vitest';
import { Check } from './check';
import { parsePlaces } from './places';

const FIELDS = ['name', 'region', 'country', 'lat_deg', 'lon_deg', 'population'];
const parse = (value: unknown): ReturnType<typeof parsePlaces> => parsePlaces(value, new Check('places.json'));
const file = (rows: unknown[]): unknown => ({ fields: FIELDS, rows });

describe('places.json', () => {
  it('reads every row in order', () => {
    expect(parse(file([['Tokyo', 'Tokyo', 'Japan', 35.687, 139.7495, 35676000], ['Singapore', '', 'Singapore', 1.295, 103.8539, 0]]))).toEqual([
      { name: 'Tokyo', region: 'Tokyo', country: 'Japan', lat_deg: 35.687, lon_deg: 139.7495, population: 35676000 },
      { name: 'Singapore', region: '', country: 'Singapore', lat_deg: 1.295, lon_deg: 103.8539, population: 0 },
    ]);
  });

  it('refuses other fields or another order', () => {
    expect(() => parse({ fields: ['name', 'country', 'region', 'lat_deg', 'lon_deg', 'population'], rows: [] })).toThrow('places.json: fields should be');
    expect(() => parse({ fields: FIELDS.slice(0, 5), rows: [] })).toThrow('fields should be');
  });

  it('refuses a row of the wrong length', () => {
    expect(() => parse(file([['Tokyo', 'Tokyo', 'Japan', 35.687, 139.7495]]))).toThrow('rows[0] should have 6 items but has 5');
  });

  it('refuses a latitude or longitude out of range, or not a number', () => {
    expect(() => parse(file([['X', '', 'Y', 90.5, 0, 1]]))).toThrow('rows[0][3] is 90.5, not a latitude');
    expect(() => parse(file([['X', '', 'Y', 0, -181, 1]]))).toThrow('rows[0][4] is -181, not a longitude');
    expect(() => parse(file([['X', '', 'Y', '0', 0, 1]]))).toThrow('rows[0][3] should be a number');
  });

  it('refuses an empty name, a name that is not a text, and a bad population', () => {
    expect(() => parse(file([[' ', '', 'Y', 0, 0, 1]]))).toThrow('rows[0][0] is an empty name');
    expect(() => parse(file([[7, '', 'Y', 0, 0, 1]]))).toThrow('rows[0][0] should be a text');
    expect(() => parse(file([['X', '', 'Y', 0, 0, 1.5]]))).toThrow('rows[0][5] should be a whole number');
    expect(() => parse(file([['X', '', 'Y', 0, 0, -1]]))).toThrow('rows[0][5] should not be negative');
    expect(() => parse(file([['X', null, 'Y', 0, 0, 1]]))).toThrow('rows[0][1] should be a text');
  });

  it('refuses a file without places', () => {
    expect(() => parse(file([]))).toThrow('rows is empty');
    expect(() => parse({ fields: FIELDS })).toThrow('rows is missing');
  });
});

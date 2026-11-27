import { describe, expect, it } from 'vitest';
import { Check } from '../data/check';
import { parsePlaces, type Place } from '../data/places';
// The repository's own file (the export copies it unchanged to places.json).
import placesText from '../../../data/ground/places/places.json?raw';
import { fold, matchKind, placeIndex, placeLabel, searchPlaces } from './place-search';

const place = (name: string, population: number, region = '', country = 'C'): Place => ({ name, region, country, lat_deg: 0, lon_deg: 0, population });

describe('folding text for matching', () => {
  it('drops accents and case, and spells out letters that do not decompose', () => {
    expect(fold('São Paulo')).toBe('sao paulo');
    expect(fold('ZÜRICH')).toBe('zurich');
    expect(fold('Reykjavík')).toBe('reykjavik');
    expect(fold('Łódź')).toBe('lodz');
    expect(fold('Tromsø')).toBe('tromso');
    expect(fold('Straße')).toBe('strasse');
    expect(fold('  Saint-Étienne ')).toBe('saint etienne');
  });
});

describe('the kind of match', () => {
  it('is a prefix, then the start of a later word, then inside a word', () => {
    expect(matchKind('new york', 'new')).toBe(0);
    expect(matchKind('new york', 'york')).toBe(1);
    expect(matchKind('new york', 'ork')).toBe(2);
    expect(matchKind('new york', 'paris')).toBe(-1);
    expect(matchKind('new york', '')).toBe(-1);
    // A later word start counts even when the text also appears inside an earlier word.
    expect(matchKind('yorkyork york', 'york')).toBe(0);
    expect(matchKind('ayork york', 'york')).toBe(1);
  });
});

describe('searching the places', () => {
  const index = placeIndex([
    place('Parisot', 900),
    place('Paris', 11_000_000, 'Ile-de-France', 'France'),
    place('Paris', 25_000, 'Texas', 'United States of America'),
    place('Comparison', 5_000_000),
    place('Le Paris Bourg', 3_000),
    place('São Paulo', 18_000_000, 'São Paulo', 'Brazil'),
    place('Zürich', 1_100_000, 'Zürich', 'Switzerland'),
  ]);
  const names = (typed: string, limit?: number): string[] => searchPlaces(index, typed, limit).map(placeLabel);

  it('puts name prefixes first, then word starts, then the rest, larger first in each', () => {
    expect(names('paris')).toEqual([
      'Paris, Ile-de-France, France',
      'Paris, Texas, United States of America',
      'Parisot, C',
      'Le Paris Bourg, C',
      'Comparison, C',
    ]);
  });

  it('ignores accents and case', () => {
    expect(names('sao paulo')).toEqual(['São Paulo, Brazil']);
    expect(names('ZURICH')).toEqual(['Zürich, Switzerland']);
    expect(names('zür')).toEqual(['Zürich, Switzerland']);
  });

  it('narrows by region or country after a comma', () => {
    expect(names('paris, texas')).toEqual(['Paris, Texas, United States of America']);
    expect(names('paris, united states')).toEqual(['Paris, Texas, United States of America']);
    expect(names('paris, fr')).toEqual(['Paris, Ile-de-France, France']);
  });

  it('gives nothing for nothing typed, and at most the limit', () => {
    expect(names('   ')).toEqual([]);
    expect(names('par', 2)).toHaveLength(2);
  });

  it('labels a place without a region, or with the name as region, without it', () => {
    expect(placeLabel(place('Singapore', 1, '', 'Singapore'))).toBe('Singapore, Singapore');
    expect(placeLabel(place('Tokyo', 1, 'Tokyo', 'Japan'))).toBe('Tokyo, Japan');
    expect(placeLabel(place('Lagos', 1, 'Lagos State', 'Nigeria'))).toBe('Lagos, Lagos State, Nigeria');
  });

  it('finds the examples of the page in the real list, largest first', () => {
    const real = placeIndex(parsePlaces(JSON.parse(placesText), new Check('places.json')));
    const first = (typed: string): string => placeLabel(searchPlaces(real, typed, 1)[0] ?? place('', 0));
    expect(first('sao paulo')).toBe('São Paulo, Brazil');
    expect(first('zurich')).toBe('Zürich, Switzerland');
    expect(first('reykjavik')).toBe('Reykjavík, Suðurnes, Iceland');
    expect(first('sydney')).toBe('Sydney, New South Wales, Australia');
    expect(first('lagos')).toBe('Lagos, Nigeria');
    expect(searchPlaces(real, 'a').length).toBe(8);
  });
});

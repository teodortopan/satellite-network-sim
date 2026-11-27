import { describe, expect, it } from 'vitest';
import { latLonToScene } from './coords';
import { PathPlacer, pointScenePosition, routeNodes, SITE_RADIUS } from './paths';
import type { SatelliteField } from './satellite-field';

/** Two satellites: 0 drawn at (5, 6, 7), 1 with no position. */
function fakeField(): SatelliteField {
  return {
    isDrawable: (sat: number) => sat === 0,
    position: { array: new Float32Array([5, 6, 7, 0, 0, 0]) },
  } as unknown as SatelliteField;
}

const SITES = new Float32Array([1, 0, 0, 0, 1, 0]);

describe('PathPlacer', () => {
  it('places saved sites by their index', () => {
    const placer = new PathPlacer(fakeField(), SITES);
    expect(placer.siteCount).toBe(2);
    expect(placer.site(1)).toEqual([0, 1, 0]);
  });

  it('numbers extra places after the saved sites', () => {
    const placer = new PathPlacer(fakeField(), SITES);
    const first = placer.setExtraSites([0, 0, 2, 0, 0, 3]);
    expect(first).toBe(2);
    expect(placer.site(2)).toEqual([0, 0, 2]);
    expect(placer.site(3)).toEqual([0, 0, 3]);
    expect(placer.site(0)).toEqual([1, 0, 0]);
  });

  it('replaces the extra places on the next call', () => {
    const placer = new PathPlacer(fakeField(), SITES);
    placer.setExtraSites([0, 0, 2, 0, 0, 3]);
    placer.setExtraSites([9, 9, 9]);
    expect(placer.site(2)).toEqual([9, 9, 9]);
    expect(() => placer.site(3)).toThrow(RangeError);
  });

  it('refuses a place that does not exist or positions that are not triples', () => {
    const placer = new PathPlacer(fakeField(), SITES);
    expect(() => placer.site(2)).toThrow(RangeError);
    expect(() => placer.site(-1)).toThrow(RangeError);
    expect(() => placer.setExtraSites([1, 2])).toThrow(RangeError);
  });

  it('draws a route between two extra places, skipping satellites with no position', () => {
    const placer = new PathPlacer(fakeField(), SITES);
    const from = placer.setExtraSites([0, 0, 2, 0, 0, 3]);
    const points = placer.points(routeNodes(from, [0, 1], from + 1));
    expect(points).toEqual([0, 0, 2, 5, 6, 7, 0, 0, 3]);
  });
});

describe('pointScenePosition', () => {
  it('is a latitude and longitude just above the ground, as the saved sites are', () => {
    const p = pointScenePosition({ lat_deg: 51.5, lon_deg: -0.1 });
    const q = latLonToScene(51.5, -0.1, SITE_RADIUS);
    p.forEach((v, i) => expect(v).toBeCloseTo(q[i] as number, 12));
    expect(Math.hypot(...p)).toBeCloseTo(SITE_RADIUS, 12);
  });
});

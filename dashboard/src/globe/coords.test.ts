import { describe, expect, it } from 'vitest';
import { EARTH_RADIUS_KM, ecefToScene, latLonToScene } from './coords';

function close(a: readonly number[], b: readonly number[]): void {
  a.forEach((v, i) => expect(v).toBeCloseTo(b[i] as number, 12));
}

describe('latitude/longitude and ECEF to the scene', () => {
  it('puts latitude 0, longitude 0 on scene +x', () => {
    close(latLonToScene(0, 0), [1, 0, 0]);
  });

  it('puts the north pole on scene +y (up)', () => {
    close(latLonToScene(90, 0), [0, 1, 0]);
    close(latLonToScene(-90, 0), [0, -1, 0]);
  });

  it('puts longitude 90 E on scene -z, so east is to the right seen from +x', () => {
    close(latLonToScene(0, 90), [0, 0, -1]);
    close(latLonToScene(0, -90), [0, 0, 1]);
  });

  it('puts London where its latitude and longitude say', () => {
    const lat = (51.5074 * Math.PI) / 180;
    const lon = (-0.1278 * Math.PI) / 180;
    close(latLonToScene(51.5074, -0.1278), [Math.cos(lat) * Math.cos(lon), Math.sin(lat), -Math.cos(lat) * Math.sin(lon)]);
  });

  it('maps ECEF kilometres with the same axes, in Earth radii', () => {
    const out = [0, 0, 0];
    ecefToScene(EARTH_RADIUS_KM, 2 * EARTH_RADIUS_KM, 3 * EARTH_RADIUS_KM, out, 0);
    close(out, [1, 3, -2]);
  });

  it('puts a satellite above Tokyo over the Tokyo marker', () => {
    const lat = (35.68 * Math.PI) / 180;
    const lon = (139.69 * Math.PI) / 180;
    const r = 6921;
    const out = [0, 0, 0];
    ecefToScene(r * Math.cos(lat) * Math.cos(lon), r * Math.cos(lat) * Math.sin(lon), r * Math.sin(lat), out, 0);
    close(
      out.map((v) => (v * EARTH_RADIUS_KM) / r),
      latLonToScene(35.68, 139.69),
    );
  });
});

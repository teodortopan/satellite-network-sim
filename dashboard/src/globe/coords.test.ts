import { describe, expect, it } from 'vitest';
import { EARTH_RADIUS_KM, ecefToScene, groundDistanceKm, latLonToScene, raySphere, sceneToLatLon } from './coords';

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

describe('a click on the globe back to latitude/longitude', () => {
  it('inverts latLonToScene everywhere, at any radius', () => {
    for (let lat = -89; lat <= 89; lat += 7.3) {
      for (let lon = -179; lon <= 179; lon += 11.9) {
        for (const radius of [1, 1.002, 3]) {
          const [x, y, z] = latLonToScene(lat, lon, radius);
          const back = sceneToLatLon(x, y, z);
          expect(back?.lat_deg).toBeCloseTo(lat, 9);
          expect(back?.lon_deg).toBeCloseTo(lon, 9);
        }
      }
    }
  });

  it('puts the poles and the four quarters where the forward conversion does', () => {
    expect(sceneToLatLon(0, 1, 0)?.lat_deg).toBeCloseTo(90, 12);
    expect(sceneToLatLon(0, -2, 0)?.lat_deg).toBeCloseTo(-90, 12);
    const at = (x: number, y: number, z: number): number[] => {
      const p = sceneToLatLon(x, y, z);
      return [p?.lat_deg ?? Number.NaN, p?.lon_deg ?? Number.NaN];
    };
    close(at(0, 0, -1), [0, 90]);
    close(at(0, 0, 1), [0, -90]);
    close(at(1, 0, 0), [0, 0]);
    close(at(-1, 0, 0).map(Math.abs), [0, 180]);
    expect(sceneToLatLon(0, 0, 0)).toBeNull();
  });

  it('finds the near side of the sphere along a ray, whatever side the camera is on', () => {
    // A camera above London, looking at the centre, hits the sphere over London.
    const london = latLonToScene(51.5, -0.13);
    const camera = london.map((v) => v * 3.9);
    const hit = raySphere(camera, camera.map((v) => -v));
    close(hit ?? [], london);
    const back = sceneToLatLon(...(hit as [number, number, number]));
    expect(back?.lat_deg).toBeCloseTo(51.5, 9);
    expect(back?.lon_deg).toBeCloseTo(-0.13, 9);
    // The same ray from the far side of the Earth meets the antipode first.
    const far = london.map((v) => -v * 3.9);
    close(raySphere(far, london) ?? [], london.map((v) => -v));
  });

  it('misses when the ray passes beside the sphere or points away', () => {
    expect(raySphere([0, 0, 5], [1, 0, 0])).toBeNull();
    expect(raySphere([0, 0, 5], [0, 0, 1])).toBeNull();
    expect(raySphere([0, 0, 5], [0, 0.5, -1])).toBeNull();
    expect(raySphere([0, 0, 5], [0, 0.1, -1])).not.toBeNull();
  });

  it('measures great-circle distances on the drawn sphere', () => {
    expect(groundDistanceKm({ lat_deg: 0, lon_deg: 0 }, { lat_deg: 0, lon_deg: 90 })).toBeCloseTo((Math.PI / 2) * EARTH_RADIUS_KM, 6);
    expect(groundDistanceKm({ lat_deg: 51.5, lon_deg: -0.13 }, { lat_deg: 51.5, lon_deg: -0.13 })).toBe(0);
    expect(groundDistanceKm({ lat_deg: 90, lon_deg: 0 }, { lat_deg: -90, lon_deg: 0 })).toBeCloseTo(Math.PI * EARTH_RADIUS_KM, 6);
  });
});

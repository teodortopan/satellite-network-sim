// Earth-fixed (ECEF) kilometres and latitude/longitude to the globe's scene coordinates.
//
// The scene is in Earth radii. three.js has y up, so the Earth's axis (ECEF z) is scene +y;
// ECEF x (latitude 0, longitude 0) is scene +x; ECEF y (longitude 90 E) is scene -z. This is
// a rotation, not a mirror, so the continents are not flipped.

export const EARTH_RADIUS_KM = 6371;

export function ecefToScene(
  x_km: number,
  y_km: number,
  z_km: number,
  out: { [k: number]: number },
  o: number,
): void {
  out[o] = x_km / EARTH_RADIUS_KM;
  out[o + 1] = z_km / EARTH_RADIUS_KM;
  out[o + 2] = -y_km / EARTH_RADIUS_KM;
}

/** A point at `radius` Earth radii above latitude/longitude (a sphere is close enough to draw). */
export function latLonToScene(lat_deg: number, lon_deg: number, radius = 1): [number, number, number] {
  const lat = (lat_deg * Math.PI) / 180;
  const lon = (lon_deg * Math.PI) / 180;
  const r = radius * EARTH_RADIUS_KM;
  const out: [number, number, number] = [0, 0, 0];
  ecefToScene(r * Math.cos(lat) * Math.cos(lon), r * Math.cos(lat) * Math.sin(lon), r * Math.sin(lat), out, 0);
  return out;
}

/** Every satellite's ECEF km to scene coordinates, in place-friendly bulk form. */
export function ecefArrayToScene(ecef: Float32Array, out: Float32Array, count: number): void {
  for (let i = 0; i < count; i++) {
    ecefToScene(ecef[3 * i] as number, ecef[3 * i + 1] as number, ecef[3 * i + 2] as number, out, 3 * i);
  }
}

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

/** The latitude and longitude (degrees) under a scene point, the inverse of latLonToScene for
 * any radius: scene +y is ECEF z, scene +x ECEF x, scene -z ECEF y. Null at the centre. */
export function sceneToLatLon(x: number, y: number, z: number): { lat_deg: number; lon_deg: number } | null {
  const r = Math.hypot(x, y, z);
  if (!(r > 0)) return null;
  const ecefX = x;
  const ecefY = -z;
  const ecefZ = y;
  const lat_deg = (Math.asin(Math.max(-1, Math.min(1, ecefZ / r))) * 180) / Math.PI;
  const lon_deg = (Math.atan2(ecefY, ecefX) * 180) / Math.PI;
  return { lat_deg, lon_deg };
}

/**
 * Where a ray (origin, then direction; scene coordinates) first meets the sphere of `radius`
 * around the centre, or null if it misses it or the sphere is behind the origin.
 */
export function raySphere(origin: readonly number[], direction: readonly number[], radius = 1): [number, number, number] | null {
  const [ox, oy, oz] = [origin[0] ?? 0, origin[1] ?? 0, origin[2] ?? 0];
  const len = Math.hypot(direction[0] ?? 0, direction[1] ?? 0, direction[2] ?? 0);
  if (!(len > 0)) return null;
  const [dx, dy, dz] = [(direction[0] ?? 0) / len, (direction[1] ?? 0) / len, (direction[2] ?? 0) / len];
  // |o + s d|^2 = r^2, with |d| = 1: s^2 + 2 b s + c = 0.
  const b = ox * dx + oy * dy + oz * dz;
  const c = ox * ox + oy * oy + oz * oz - radius * radius;
  const disc = b * b - c;
  if (disc < 0) return null;
  const root = Math.sqrt(disc);
  const near = -b - root;
  const s = near >= 0 ? near : -b + root;
  if (s < 0) return null;
  return [ox + s * dx, oy + s * dy, oz + s * dz];
}

/** The great-circle distance between two places on the drawn sphere of EARTH_RADIUS_KM, in km. */
export function groundDistanceKm(a: { lat_deg: number; lon_deg: number }, b: { lat_deg: number; lon_deg: number }): number {
  const rad = Math.PI / 180;
  const dLat = (b.lat_deg - a.lat_deg) * rad;
  const dLon = (b.lon_deg - a.lon_deg) * rad;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat_deg * rad) * Math.cos(b.lat_deg * rad) * Math.sin(dLon / 2) ** 2;
  return 2 * EARTH_RADIUS_KM * Math.asin(Math.min(1, Math.sqrt(h)));
}

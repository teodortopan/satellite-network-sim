// Positions between two samples, exactly as spec 4.3 (and dashboard/data-format.md) say. It is
// the only arithmetic the page does on simulation data, and it matches the data check's
// Python version (python/check_dashboard_data.py, interpolate) to rounding; the unit test
// holds it to the numbers python/make_interpolation_reference.py wrote.

export const EARTH_RATE_RAD_S = 7.292115e-5;

/**
 * The position at share `share` of the step from sample p0 (at a[i0..i0+2]) to sample p1
 * (at a[i1..i1+2]) that are `dt_s` apart, written to out[o..o+2] in ECEF km. Returns false,
 * writing nothing, when either sample is NaN: that satellite is not drawn for this step.
 */
export function interpolateAt(
  a: ArrayLike<number>,
  i0: number,
  i1: number,
  dt_s: number,
  share: number,
  out: { [k: number]: number },
  o: number,
): boolean {
  const x0 = a[i0] as number;
  const y0 = a[i0 + 1] as number;
  const z0 = a[i0 + 2] as number;
  const x1 = a[i1] as number;
  const y1 = a[i1 + 1] as number;
  const z1 = a[i1 + 2] as number;
  if (Number.isNaN(x0 + y0 + z0 + x1 + y1 + z1)) return false;
  // At a sample time the result is the sample itself.
  if (share === 0) {
    out[o] = x0;
    out[o + 1] = y0;
    out[o + 2] = z0;
    return true;
  }
  if (share === 1) {
    out[o] = x1;
    out[o + 1] = y1;
    out[o + 2] = z1;
    return true;
  }
  // 1. Turn the later sample back by the Earth's turn over the step, into t0's axes.
  const back = EARTH_RATE_RAD_S * dt_s;
  const cb = Math.cos(back);
  const sb = Math.sin(back);
  const qx = cb * x1 - sb * y1;
  const qy = sb * x1 + cb * y1;
  const qz = z1;
  // 2. Great-circle mix of the two directions, straight mix of the two distances.
  const r0 = Math.hypot(x0, y0, z0);
  const r1 = Math.hypot(qx, qy, qz);
  const ux0 = x0 / r0;
  const uy0 = y0 / r0;
  const uz0 = z0 / r0;
  const ux1 = qx / r1;
  const uy1 = qy / r1;
  const uz1 = qz / r1;
  const cx = uy0 * uz1 - uz0 * uy1;
  const cy = uz0 * ux1 - ux0 * uz1;
  const cz = ux0 * uy1 - uy0 * ux1;
  const angle = Math.atan2(Math.hypot(cx, cy, cz), ux0 * ux1 + uy0 * uy1 + uz0 * uz1);
  const sinAngle = Math.sin(angle);
  // Same directions: the mix is a straight line (the Python version's small-angle branch).
  const small = sinAngle < 1e-12;
  const wa = small ? 1 - share : Math.sin((1 - share) * angle) / sinAngle;
  const wb = small ? share : Math.sin(share * angle) / sinAngle;
  let dx = wa * ux0 + wb * ux1;
  let dy = wa * uy0 + wb * uy1;
  let dz = wa * uz0 + wb * uz1;
  const norm = Math.hypot(dx, dy, dz);
  const r = (1 - share) * r0 + share * r1;
  dx = (dx / norm) * r;
  dy = (dy / norm) * r;
  dz = (dz / norm) * r;
  // 3. Turn forward by the Earth's turn from t0 to t.
  const fwd = -EARTH_RATE_RAD_S * share * dt_s;
  const cf = Math.cos(fwd);
  const sf = Math.sin(fwd);
  out[o] = cf * dx - sf * dy;
  out[o + 1] = sf * dx + cf * dy;
  out[o + 2] = dz;
  return true;
}

export interface SamplePair {
  /** The earlier sample; the later one is s0 + 1. */
  s0: number;
  share: number;
}

/** Which two samples enclose window time `t_s`, and how far between them it is. */
export function samplePair(t_s: number, step_s: number, samples: number): SamplePair {
  const last = (samples - 1) * step_s;
  if (!(t_s >= 0 && t_s <= last)) {
    throw new RangeError(`time ${t_s} s is outside the window 0 to ${last} s`);
  }
  const s0 = Math.min(Math.floor(t_s / step_s), samples - 2);
  return { s0, share: (t_s - s0 * step_s) / step_s };
}

/**
 * Every satellite's position at window time `t_s` into `out` (ECEF km, 3 per satellite), and
 * `drawable[i]` = 1 where satellite i has both samples, 0 where it is not drawn.
 */
export function positionsAt(
  samples: Float32Array,
  satellites: number,
  sampleCount: number,
  step_s: number,
  t_s: number,
  out: Float32Array,
  drawable: Uint8Array,
): void {
  const { s0, share } = samplePair(t_s, step_s, sampleCount);
  const base0 = s0 * satellites * 3;
  const base1 = base0 + satellites * 3;
  for (let i = 0; i < satellites; i++) {
    drawable[i] = interpolateAt(samples, base0 + 3 * i, base1 + 3 * i, step_s, share, out, 3 * i)
      ? 1
      : 0;
  }
}

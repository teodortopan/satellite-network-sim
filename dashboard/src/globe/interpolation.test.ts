import { describe, expect, it } from 'vitest';
import reference from './interpolation-reference.json';
import { EARTH_RATE_RAD_S, interpolateAt, positionsAt, samplePair } from './interpolation';

const TOLERANCE_KM = 1e-9;

function at(p0: number[], p1: number[], dt_s: number, share: number): number[] | null {
  const out = [0, 0, 0];
  return interpolateAt([...p0, ...p1], 0, 3, dt_s, share, out, 0) ? out : null;
}

describe('interpolation (spec 4.3)', () => {
  it('uses the rotation rate of the Python version', () => {
    expect(EARTH_RATE_RAD_S).toBe(reference.earth_rate_rad_s);
  });

  it.each(reference.cases.map((c, i) => [i, c] as const))('equals the reference numbers, case %i', (_i, c) => {
    const out = at(c.p0_km, c.p1_km, c.dt_s, c.share);
    expect(out).not.toBeNull();
    out?.forEach((v, k) => {
      expect(Math.abs(v - (c.expected_km[k] as number))).toBeLessThan(TOLERANCE_KM);
    });
  });

  it('equals the samples at the sample times', () => {
    const p0 = [6815.85, 1201.82, 0];
    const p1 = [6759.92, 1439.33, 363.39];
    expect(at(p0, p1, 60, 0)).toEqual(p0);
    expect(at(p0, p1, 60, 1)).toEqual(p1);
  });

  it('draws nothing when a sample is NaN', () => {
    expect(at([Number.NaN, Number.NaN, Number.NaN], [7000, 0, 0], 60, 0.5)).toBeNull();
    expect(at([7000, 0, 0], [7000, Number.NaN, 0], 60, 0)).toBeNull();
  });

  it('turns the later sample back by the Earth rotation (a fixed point in space stays on its arc)', () => {
    // A point fixed in space, seen from the turning Earth: midway it is turned back by half a step.
    const r = 7000;
    const dt = 60;
    const turn = EARTH_RATE_RAD_S * dt;
    const p1 = [r * Math.cos(-turn), r * Math.sin(-turn), 0];
    const mid = at([r, 0, 0], p1, dt, 0.5) as number[];
    expect(mid[0]).toBeCloseTo(r * Math.cos(-turn / 2), 6);
    expect(mid[1]).toBeCloseTo(r * Math.sin(-turn / 2), 6);
  });

  it('picks the two samples around a window time', () => {
    expect(samplePair(0, 60, 8)).toEqual({ s0: 0, share: 0 });
    expect(samplePair(90, 60, 8)).toEqual({ s0: 1, share: 0.5 });
    expect(samplePair(420, 60, 8)).toEqual({ s0: 6, share: 1 });
    expect(() => samplePair(421, 60, 8)).toThrow(RangeError);
    expect(() => samplePair(-1, 60, 8)).toThrow(RangeError);
  });

  it('moves every satellite and marks the ones with no position', () => {
    const samples = new Float32Array([7000, 0, 0, Number.NaN, Number.NaN, Number.NaN, 7000, 0, 0, 0, 7000, 0]);
    const out = new Float32Array(6);
    const drawable = new Uint8Array(2);
    positionsAt(samples, 2, 2, 60, 0, out, drawable);
    expect(Array.from(drawable)).toEqual([1, 0]);
    expect(Array.from(out.subarray(0, 3))).toEqual([7000, 0, 0]);
  });
});

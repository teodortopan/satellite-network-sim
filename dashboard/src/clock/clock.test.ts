import { describe, expect, it } from 'vitest';
import { Clock, MAX_SPEED, MIN_SPEED } from './clock';
import { sliderToSpeed, speedToSlider } from './clock-view';

describe('clock', () => {
  it('stands still until played, and while paused', () => {
    const c = new Clock({ duration_s: 100, speed: 10 });
    c.tick(1);
    expect(c.t).toBe(0);
    c.play();
    c.tick(1);
    expect(c.t).toBe(10);
    c.pause();
    c.tick(1);
    expect(c.t).toBe(10);
    expect(c.effectiveSpeed).toBe(0);
  });

  it('plays at the chosen speed, kept between 1x and 600x', () => {
    const c = new Clock({ duration_s: 10000, speed: 60 });
    c.play();
    c.tick(0.5);
    expect(c.t).toBe(30);
    c.setSpeed(5000);
    expect(c.speed).toBe(MAX_SPEED);
    c.setSpeed(0.001);
    expect(c.speed).toBe(MIN_SPEED);
  });

  it('never leaves the window', () => {
    const c = new Clock({ duration_s: 100, speed: 600 });
    c.play();
    c.tick(10);
    expect(c.t).toBe(100);
    expect(c.playing).toBe(false);
    c.seek(-5);
    expect(c.t).toBe(0);
    c.seek(500);
    expect(c.t).toBe(100);
    c.play();
    expect(c.t).toBe(0);
  });

  it('plays a slow-motion span at the smaller of the chosen and the span speed', () => {
    const spans = [
      { from_s: 67, to_s: 68, speed: 0.02 },
      { from_s: 68, to_s: 210, speed: 2 },
    ];
    const c = new Clock({ duration_s: 367, spans, speed: 10 });
    c.seek(67);
    c.play();
    expect(c.span).toEqual(spans[0]);
    expect(c.effectiveSpeed).toBe(0.02);
    c.tick(10);
    expect(c.t).toBeCloseTo(67.2, 12);
    c.seek(100);
    expect(c.effectiveSpeed).toBe(2);
    c.setSpeed(1);
    expect(c.effectiveSpeed).toBe(1);
    c.seek(300);
    c.setSpeed(10);
    expect(c.span).toBeNull();
    expect(c.effectiveSpeed).toBe(10);
  });

  it('does not jump over a span in one long tick', () => {
    const c = new Clock({ duration_s: 367, spans: [{ from_s: 67, to_s: 68, speed: 0.02 }], speed: 600 });
    c.seek(60);
    c.play();
    // 60 -> 67 takes 7/600 s; the remaining real time plays at 0.02x.
    c.tick(0.5);
    expect(c.t).toBeCloseTo(67 + (0.5 - 7 / 600) * 0.02, 12);
  });

  it('holds before the failure for the given real time, once per pass', () => {
    const c = new Clock({ duration_s: 367, hold: { at_s: 66.95, real_s: 2 }, speed: 10 });
    c.seek(60);
    c.play();
    c.tick(1);
    expect(c.t).toBe(66.95);
    expect(c.holding).toBe(true);
    c.tick(1.5);
    expect(c.t).toBe(66.95);
    c.tick(1);
    expect(c.holding).toBe(false);
    // 0.695 s to reach the hold, 2 s of hold, so 3.5 - 2.695 = 0.805 s played after it.
    expect(c.t).toBeCloseTo(66.95 + 0.805 * 10, 10);
    // Past it, no second hold; going back before it arms it again.
    c.seek(70);
    c.tick(1);
    expect(c.t).toBe(80);
    c.seek(66);
    c.tick(1);
    expect(c.t).toBe(66.95);
    expect(c.holding).toBe(true);
  });

  it('moving the time bar always works, also while holding', () => {
    const c = new Clock({ duration_s: 367, hold: { at_s: 66.95, real_s: 2 }, speed: 10 });
    c.seek(66.9);
    c.play();
    c.tick(0.1);
    expect(c.holding).toBe(true);
    c.seek(200);
    expect(c.t).toBe(200);
    expect(c.holding).toBe(false);
  });

  it('maps the speed slider onto 1x to 600x', () => {
    expect(sliderToSpeed(0)).toBe(1);
    expect(sliderToSpeed(100)).toBe(600);
    expect(sliderToSpeed(speedToSlider(60))).toBeCloseTo(60, -1);
  });
});

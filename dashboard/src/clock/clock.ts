// The story clock (spec 4.4): one simulated time, played at a chosen speed, slowed inside a
// story's slow-motion spans, held for a while just before an instant the story names, and
// never outside the window [0, duration_s]. Pure logic: the page calls tick() with real
// seconds; nothing here touches the DOM or the real clock.

export const MIN_SPEED = 1;
export const MAX_SPEED = 600;

export interface Span {
  /** [from_s, to_s) in story time, played at no more than `speed` simulated s per real s. */
  from_s: number;
  to_s: number;
  speed: number;
}

export interface Hold {
  /** Story time at which the playing clock stops for `real_s` real seconds, once per pass. */
  at_s: number;
  real_s: number;
}

export interface ClockOptions {
  duration_s: number;
  spans?: readonly Span[];
  hold?: Hold;
  speed?: number;
}

export class Clock {
  readonly duration_s: number;
  private readonly spans: readonly Span[];
  private readonly holdPoint: Hold | null;
  private time = 0;
  private chosen: number;
  private isPlaying = false;
  private holdArmed: boolean;
  private holdLeft_s = 0;

  constructor(options: ClockOptions) {
    if (!(options.duration_s > 0)) throw new RangeError('a clock needs a positive duration');
    this.duration_s = options.duration_s;
    this.spans = [...(options.spans ?? [])].sort((a, b) => a.from_s - b.from_s);
    for (const s of this.spans) {
      if (!(s.speed > 0) || !(s.to_s > s.from_s)) throw new RangeError('a slow-motion span is empty');
    }
    this.holdPoint = options.hold ?? null;
    this.holdArmed = this.holdPoint !== null;
    this.chosen = clampSpeed(options.speed ?? MIN_SPEED);
  }

  get t(): number {
    return this.time;
  }

  get playing(): boolean {
    return this.isPlaying;
  }

  /** The speed the visitor chose. */
  get speed(): number {
    return this.chosen;
  }

  /** The slow-motion span the clock is in, if any. */
  get span(): Span | null {
    return this.spanAt(this.time);
  }

  /** True while the clock waits at the hold point. */
  get holding(): boolean {
    return this.holdLeft_s > 0;
  }

  /** Simulated seconds per real second right now (0 while paused or holding). */
  get effectiveSpeed(): number {
    if (!this.isPlaying || this.holding) return 0;
    return this.speedAt(this.time);
  }

  play(): void {
    if (this.time >= this.duration_s) this.seek(0);
    this.isPlaying = true;
  }

  pause(): void {
    this.isPlaying = false;
  }

  setSpeed(speed: number): void {
    this.chosen = clampSpeed(speed);
  }

  /** Moving the time bar by hand: always allowed, kept inside the window. */
  seek(t_s: number): void {
    this.time = Math.min(Math.max(t_s, 0), this.duration_s);
    this.holdLeft_s = 0;
    this.holdArmed = this.holdPoint !== null && this.time < this.holdPoint.at_s;
  }

  /** Advance by `real_s` real seconds of playing. */
  tick(real_s: number): void {
    let left = real_s;
    while (this.isPlaying && left > 0) {
      if (this.holdLeft_s > 0) {
        const used = Math.min(left, this.holdLeft_s);
        this.holdLeft_s -= used;
        left -= used;
        continue;
      }
      if (this.time >= this.duration_s) {
        this.isPlaying = false;
        break;
      }
      const speed = this.speedAt(this.time);
      const next = this.nextBoundary(this.time);
      const needed = (next - this.time) / speed;
      if (needed <= left) {
        this.time = next;
        left -= needed;
        if (this.holdArmed && this.holdPoint !== null && next === this.holdPoint.at_s) {
          this.holdArmed = false;
          this.holdLeft_s = this.holdPoint.real_s;
        }
      } else {
        this.time += left * speed;
        left = 0;
      }
    }
    if (this.time >= this.duration_s) this.isPlaying = false;
  }

  private spanAt(t: number): Span | null {
    return this.spans.find((s) => s.from_s <= t && t < s.to_s) ?? null;
  }

  private speedAt(t: number): number {
    const span = this.spanAt(t);
    return span === null ? this.chosen : Math.min(this.chosen, span.speed);
  }

  /** The first instant after t where the speed may change, the clock may hold, or the window ends. */
  private nextBoundary(t: number): number {
    let next = this.duration_s;
    for (const s of this.spans) {
      if (s.from_s > t) next = Math.min(next, s.from_s);
      if (s.to_s > t) next = Math.min(next, s.to_s);
    }
    if (this.holdArmed && this.holdPoint !== null && this.holdPoint.at_s > t) {
      next = Math.min(next, this.holdPoint.at_s);
    }
    return next;
  }
}

function clampSpeed(speed: number): number {
  return Math.min(Math.max(speed, MIN_SPEED), MAX_SPEED);
}

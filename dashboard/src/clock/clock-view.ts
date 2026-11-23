// The clock's controls: play/pause, the time bar with the story's marks and slow-motion
// spans under it, the speed slider, and a readout of the time and the speed actually played.

import { formatClock, formatSpeed } from '../format';
import { MAX_SPEED, MIN_SPEED, type Clock, type Span } from './clock';

export interface Mark {
  t_s: number;
  label: string;
}

const SLIDER_STEPS = 100;

/** Slider position (0..SLIDER_STEPS) to speed, evenly on a log scale from 1x to 600x. */
export function sliderToSpeed(position: number): number {
  return Math.round(MIN_SPEED * (MAX_SPEED / MIN_SPEED) ** (position / SLIDER_STEPS));
}

export function speedToSlider(speed: number): number {
  return Math.round((SLIDER_STEPS * Math.log(speed / MIN_SPEED)) / Math.log(MAX_SPEED / MIN_SPEED));
}

export class ClockView {
  readonly element: HTMLElement;
  private readonly playButton: HTMLButtonElement;
  private readonly bar: HTMLInputElement;
  private readonly time: HTMLElement;
  private readonly speedInput: HTMLInputElement;
  private readonly speedLabel: HTMLElement;
  private readonly status: HTMLElement;
  private readonly holdLabel: string;
  private dragging = false;

  constructor(
    private readonly clock: Clock,
    options: { marks: readonly Mark[]; spans: readonly Span[]; holdLabel: string },
  ) {
    const el = document.createElement('div');
    el.className = 'clock';
    el.innerHTML = `
      <button type="button" class="clock-play"></button>
      <div class="clock-time" aria-live="off"></div>
      <div class="clock-bar">
        <div class="clock-track" aria-hidden="true"></div>
        <input type="range" class="clock-range" min="0" step="any" aria-label="Time in the run">
      </div>
      <label class="clock-speed">
        <span class="clock-speed-label"></span>
        <input type="range" min="0" max="${SLIDER_STEPS}" step="1" aria-label="Playback speed">
      </label>
      <div class="clock-status" role="status"></div>`;
    this.element = el;
    this.playButton = el.querySelector('.clock-play') as HTMLButtonElement;
    this.bar = el.querySelector('.clock-range') as HTMLInputElement;
    this.time = el.querySelector('.clock-time') as HTMLElement;
    this.speedInput = el.querySelector('.clock-speed input') as HTMLInputElement;
    this.speedLabel = el.querySelector('.clock-speed-label') as HTMLElement;
    this.status = el.querySelector('.clock-status') as HTMLElement;
    this.holdLabel = options.holdLabel;

    this.bar.max = String(clock.duration_s);
    const track = el.querySelector('.clock-track') as HTMLElement;
    const at = (t: number): string => `${(100 * t) / clock.duration_s}%`;
    for (const span of options.spans) {
      const band = document.createElement('div');
      band.className = span.speed < 1 ? 'clock-span deep' : 'clock-span';
      band.style.left = at(span.from_s);
      band.style.width = `calc(${at(span.to_s - span.from_s)} + 1px)`;
      band.title = `slow motion, at most ${formatSpeed(span.speed)}`;
      track.append(band);
    }
    for (const mark of options.marks) {
      const tick = document.createElement('div');
      tick.className = 'clock-mark';
      tick.style.left = at(mark.t_s);
      tick.dataset.label = mark.label;
      track.append(tick);
    }

    this.playButton.addEventListener('click', () => {
      if (clock.playing) clock.pause();
      else clock.play();
      this.update();
    });
    this.bar.addEventListener('pointerdown', () => (this.dragging = true));
    window.addEventListener('pointerup', this.release);
    this.bar.addEventListener('input', () => {
      clock.seek(Number(this.bar.value));
      this.update();
    });
    this.speedInput.value = String(speedToSlider(clock.speed));
    this.speedInput.addEventListener('input', () => {
      clock.setSpeed(sliderToSpeed(Number(this.speedInput.value)));
      this.update();
    });
    this.update();
  }

  private readonly release = (): void => {
    this.dragging = false;
  };

  /** Bring every readout in line with the clock (called every frame). */
  update(): void {
    const c = this.clock;
    this.playButton.textContent = c.playing ? 'Pause' : 'Play';
    this.playButton.setAttribute('aria-pressed', String(c.playing));
    if (!this.dragging) this.bar.value = String(c.t);
    this.time.textContent = formatClock(c.t, c.span !== null);
    this.element.dataset.t = String(c.t);
    this.speedLabel.textContent = formatSpeed(c.speed);
    let status = '';
    if (c.holding) status = this.holdLabel;
    else if (c.span !== null) status = `slow motion ${formatSpeed(Math.min(c.speed, c.span.speed))}`;
    this.status.textContent = status;
    this.element.dataset.slowMotion = String(status.startsWith('slow motion'));
  }

  dispose(): void {
    window.removeEventListener('pointerup', this.release);
    this.element.remove();
  }
}

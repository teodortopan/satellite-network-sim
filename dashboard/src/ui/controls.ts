// The controls the stories share: a row of buttons where one is chosen (a scenario, a
// direction, a pattern), a switch that is on or off (a strategy, a kind of ground site), and
// the animation loop that draws a story's frames.

import type { Mark } from '../theme';
import { setSwatch } from './marks';

export interface Choice {
  /** Written to data-value, for the browser test and the CSS. */
  value: string;
  text: string;
}

export interface Segmented {
  element: HTMLElement;
  select(index: number): void;
}

/** Buttons of which exactly one is chosen; `onPick` runs on a click (not on select()). */
export function segmented(label: string, choices: readonly Choice[], onPick: (index: number) => void): Segmented {
  const element = document.createElement('div');
  element.className = 'segmented';
  element.setAttribute('role', 'radiogroup');
  element.setAttribute('aria-label', label);
  const buttons = choices.map((choice, i) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.setAttribute('role', 'radio');
    button.dataset.value = choice.value;
    button.textContent = choice.text;
    button.addEventListener('click', () => {
      select(i);
      onPick(i);
    });
    element.append(button);
    return button;
  });
  const select = (index: number): void => {
    buttons.forEach((b, i) => b.setAttribute('aria-checked', String(i === index)));
  };
  return { element, select };
}

/** A switch with the swatch of what it shows; `onChange` runs with the new state on a click. */
export function toggle(choice: Choice, on: boolean, swatch: Mark | null, onChange: (on: boolean) => void): HTMLButtonElement {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'toggle';
  button.dataset.value = choice.value;
  button.setAttribute('aria-pressed', String(on));
  button.textContent = choice.text;
  setSwatch(button, swatch);
  button.addEventListener('click', () => {
    const next = button.getAttribute('aria-pressed') !== 'true';
    button.setAttribute('aria-pressed', String(next));
    onChange(next);
  });
  return button;
}

/** A labelled drop-down list; `onPick` runs with the chosen index. */
export function dropdown(label: string, choices: readonly Choice[], onPick: (index: number) => void): HTMLLabelElement {
  const wrap = document.createElement('label');
  wrap.className = 'dropdown';
  const text = document.createElement('span');
  text.textContent = label;
  const select = document.createElement('select');
  for (const choice of choices) {
    const option = document.createElement('option');
    option.value = choice.value;
    option.textContent = choice.text;
    select.append(option);
  }
  select.addEventListener('change', () => onPick(select.selectedIndex));
  wrap.append(text, select);
  return wrap;
}

/** Calls `frame` with the real seconds since the last frame (at most 0.25 s) until stopped. */
export function frameLoop(frame: (real_s: number) => void): () => void {
  let id = 0;
  let last = performance.now();
  const loop = (now: number): void => {
    const real_s = Math.min((now - last) / 1000, 0.25);
    last = now;
    frame(real_s);
    id = requestAnimationFrame(loop);
  };
  id = requestAnimationFrame(loop);
  return () => cancelAnimationFrame(id);
}

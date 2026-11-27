// What every story shows first, so a visitor knows within seconds what they look at and what
// to press: a plain title, one or two sentences, a "What to try" line, and two folded parts
// ("How to read this" with the marks, "Details and sources" with the files and settings).
// Also the small legend beside each globe.

import type { Mark } from '../theme';
import { setSwatch } from './marks';

/** The top of a story, with empty slots the story fills: .lede, .try-text, .how-body, .sources-body. */
export function introMarkup(title: string): string {
  return `
      <header class="intro">
        <h1>${title}</h1>
        <div class="intro-text">
          <p class="lede"></p>
          <p class="try"><span class="try-label">What to try:</span> <span class="try-text"></span></p>
          <div class="folds">
            <details class="fold how-to-read"><summary>How to read this</summary><div class="fold-body how-body"></div></details>
            <details class="fold sources"><summary>Details and sources</summary><div class="fold-body sources-body"></div></details>
          </div>
        </div>
      </header>`;
}

/** Paragraphs of plain text into a fold's body (each string one paragraph). */
export function fillFold(body: HTMLElement, paragraphs: readonly (string | Node)[]): void {
  body.replaceChildren(
    ...paragraphs.map((text) => {
      if (typeof text !== 'string') return text;
      const p = document.createElement('p');
      p.textContent = text;
      return p;
    }),
  );
}

/** A list of marks with what each one means: the legend beside a globe (or a fold). */
export function legendList(items: readonly (readonly [Mark, string])[], className = 'globe-legend'): HTMLUListElement {
  const list = document.createElement('ul');
  list.className = className;
  list.setAttribute('aria-label', 'What the marks mean');
  for (const [mark, text] of items) {
    const li = document.createElement('li');
    li.textContent = text;
    setSwatch(li, mark);
    list.append(li);
  }
  return list;
}

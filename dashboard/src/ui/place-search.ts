// A box to find a place by name: a text field with a list of suggestions under it (an ARIA
// combobox). The matching is a pure function (searchPlaces) over an index built once: name
// prefix first, then the start of a word in the name, then anywhere in the name; accents and
// case ignored; larger places first within each kind of match.

import type { Place } from '../data/places';

/** Text folded for matching: no accents, lower case, letters that do not decompose spelled out. */
export function fold(text: string): string {
  return text
    .normalize('NFD')
    .replace(/\p{M}/gu, '')
    .toLowerCase()
    .replace(/[ßøłđæœıþð’‘`´]/g, (ch) => FOLDS[ch] ?? ch)
    .replace(/[\s\-‐–—]+/g, ' ')
    .trim();
}

const FOLDS: Record<string, string> = {
  ß: 'ss',
  ø: 'o',
  ł: 'l',
  đ: 'd',
  æ: 'ae',
  œ: 'oe',
  ı: 'i',
  þ: 'th',
  ð: 'd',
  '’': "'",
  '‘': "'",
  '`': "'",
  '´': "'",
};

export interface PlaceIndex {
  places: readonly Place[];
  /** Each place's folded name. */
  names: string[];
  /** Each place's folded region and country, for a query "name, country". */
  rest: string[];
}

export function placeIndex(places: readonly Place[]): PlaceIndex {
  return {
    places,
    names: places.map((p) => fold(p.name)),
    rest: places.map((p) => `${fold(p.region)} ${fold(p.country)}`),
  };
}

/** How a folded name matches a folded query: 0 prefix, 1 start of a later word, 2 inside, -1 not. */
export function matchKind(name: string, query: string): number {
  if (query === '') return -1;
  if (name.startsWith(query)) return 0;
  const at = name.indexOf(query);
  if (at < 0) return -1;
  for (let i = at; i >= 0; i = name.indexOf(query, i + 1)) {
    const before = name[i - 1] ?? ' ';
    if (before === ' ' || before === "'" || before === '(' || before === '.' || before === '/') return 1;
  }
  return 2;
}

/**
 * The best `limit` places for what was typed. "Name, more" also asks that the words after the
 * comma start a word of the region or country ("london, canada"). Ties: larger population
 * first, then the file's order (largest first).
 */
export function searchPlaces(index: PlaceIndex, typed: string, limit = 8): Place[] {
  const comma = typed.indexOf(',');
  const query = fold(comma < 0 ? typed : typed.slice(0, comma));
  const qualifier = comma < 0 ? '' : fold(typed.slice(comma + 1).replaceAll(',', ' '));
  if (query === '') return [];
  const found: { i: number; kind: number }[] = [];
  index.names.forEach((name, i) => {
    const kind = matchKind(name, query);
    if (kind < 0) return;
    if (qualifier !== '' && !qualifier.split(' ').every((w) => matchKind(index.rest[i] ?? '', w) === 0 || matchKind(index.rest[i] ?? '', w) === 1)) return;
    found.push({ i, kind });
  });
  const pop = (i: number): number => index.places[i]?.population ?? 0;
  found.sort((a, b) => a.kind - b.kind || pop(b.i) - pop(a.i) || a.i - b.i);
  return found.slice(0, limit).map((f) => index.places[f.i] as Place);
}

/** "Name, Region, Country", without the region when it is empty or the name itself. */
export function placeLabel(p: Pick<Place, 'name' | 'region' | 'country'>): string {
  const parts = [p.name];
  if (p.region.trim() !== '' && fold(p.region) !== fold(p.name)) parts.push(p.region);
  if (p.country.trim() !== '') parts.push(p.country);
  return parts.join(', ');
}

/** What a box holds once a place is chosen: a listed place or a point picked on the globe. */
export interface Chosen {
  /** The short name: the headline, the globe, the engine's file. */
  name: string;
  /** What the box shows. */
  label: string;
  lat_deg: number;
  lon_deg: number;
}

/** A listed place in a box: its name and country (the list under the box shows the region too). */
export function chosenFrom(p: Place): Chosen {
  const label = p.country.trim() === '' || fold(p.country) === fold(p.name) ? p.name : `${p.name}, ${p.country}`;
  return { name: p.name, label, lat_deg: p.lat_deg, lon_deg: p.lon_deg };
}

let boxes = 0;

/**
 * One place box: a labelled text field (role combobox) and its suggestion list (role listbox).
 * Arrow keys move through the suggestions, Enter takes the highlighted one (or the first),
 * Escape closes the list; a click or tap on a suggestion takes it.
 */
export class PlaceBox {
  readonly element: HTMLElement;
  readonly input: HTMLInputElement;
  private readonly list: HTMLUListElement;
  private shown: Place[] = [];
  private active = -1;
  private value: Chosen | null = null;

  constructor(
    label: string,
    private readonly index: PlaceIndex,
    private readonly events: {
      /** A place was chosen (from the list or by set()). */
      onChoose: (box: PlaceBox, chosen: Chosen) => void;
      /** The box got the focus: it is the one the globe sets now. */
      onFocus: (box: PlaceBox) => void;
      /** Enter in the box with nothing to choose from the list. */
      onSubmit: () => void;
    },
  ) {
    const id = `place-box-${++boxes}`;
    this.element = document.createElement('div');
    this.element.className = 'place-box';
    this.element.innerHTML = `
      <label class="place-label" for="${id}"></label>
      <div class="place-field">
        <input id="${id}" class="place-input" type="text" role="combobox" autocomplete="off" autocapitalize="off"
          spellcheck="false" aria-autocomplete="list" aria-expanded="false" aria-controls="${id}-list" />
        <ul id="${id}-list" class="place-suggestions" role="listbox" hidden></ul>
      </div>`;
    (this.element.querySelector('label') as HTMLElement).textContent = label;
    this.input = this.element.querySelector('input') as HTMLInputElement;
    this.input.placeholder = 'Type a city or town';
    this.list = this.element.querySelector('ul') as HTMLUListElement;
    this.list.setAttribute('aria-label', `Places for ${label}`);

    this.input.addEventListener('input', () => {
      this.value = null;
      this.element.dataset.chosen = 'false';
      this.suggest();
    });
    this.input.addEventListener('focus', () => {
      this.events.onFocus(this);
      this.input.select();
    });
    this.input.addEventListener('blur', () => {
      this.close();
      // Leaving a half-typed box: show the place it still holds, if any.
      if (this.value !== null) this.input.value = this.value.label;
    });
    this.input.addEventListener('keydown', (e) => this.key(e));
    // Taken on pointerdown, before the field loses the focus (which closes the list).
    this.list.addEventListener('pointerdown', (e) => {
      const li = (e.target as HTMLElement).closest('li');
      if (li === null) return;
      e.preventDefault();
      this.take(Number(li.dataset.index));
    });
  }

  /** The chosen place, or null while the text is not a chosen place. */
  get chosen(): Chosen | null {
    return this.value;
  }

  /** The best match of what is typed (for a press of the button without choosing). */
  get bestGuess(): Place | null {
    return searchPlaces(this.index, this.input.value, 1)[0] ?? null;
  }

  set(chosen: Chosen | null, notify = false): void {
    this.value = chosen;
    this.input.value = chosen?.label ?? '';
    this.element.dataset.chosen = String(chosen !== null);
    this.close();
    if (chosen !== null && notify) this.events.onChoose(this, chosen);
  }

  setActive(on: boolean): void {
    this.element.dataset.active = String(on);
  }

  private suggest(): void {
    this.shown = searchPlaces(this.index, this.input.value);
    this.active = -1;
    this.list.replaceChildren(
      ...this.shown.map((p, i) => {
        const li = document.createElement('li');
        li.id = `${this.input.id}-option-${i}`;
        li.setAttribute('role', 'option');
        li.setAttribute('aria-selected', 'false');
        li.dataset.index = String(i);
        li.textContent = placeLabel(p);
        return li;
      }),
    );
    if (this.shown.length === 0 && this.input.value.trim() !== '') {
      const li = document.createElement('li');
      li.className = 'place-none';
      li.textContent = 'No place of that name in the list. Try another spelling, or click the globe.';
      this.list.append(li);
    }
    const open = this.list.children.length > 0;
    this.list.hidden = !open;
    this.input.setAttribute('aria-expanded', String(open && this.shown.length > 0));
    this.input.removeAttribute('aria-activedescendant');
  }

  private highlight(i: number): void {
    this.active = i;
    this.list.querySelectorAll<HTMLElement>('li[role="option"]').forEach((li, k) => {
      li.setAttribute('aria-selected', String(k === i));
      if (k === i) li.scrollIntoView({ block: 'nearest' });
    });
    if (i >= 0) this.input.setAttribute('aria-activedescendant', `${this.input.id}-option-${i}`);
    else this.input.removeAttribute('aria-activedescendant');
  }

  private key(e: KeyboardEvent): void {
    const open = !this.list.hidden && this.shown.length > 0;
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      if (!open) {
        this.suggest();
        if (this.shown.length === 0) return;
      }
      const n = this.shown.length;
      const step = e.key === 'ArrowDown' ? 1 : -1;
      this.highlight(this.active < 0 ? (step > 0 ? 0 : n - 1) : (this.active + step + n) % n);
    } else if (e.key === 'Enter') {
      e.preventDefault();
      if (open) this.take(this.active < 0 ? 0 : this.active);
      else this.events.onSubmit();
    } else if (e.key === 'Escape') {
      if (open || !this.list.hidden) {
        e.preventDefault();
        this.close();
      }
    }
  }

  private take(i: number): void {
    const p = this.shown[i];
    if (p === undefined) return;
    this.set(chosenFrom(p), true);
  }

  private close(): void {
    this.list.hidden = true;
    this.input.setAttribute('aria-expanded', 'false');
    this.input.removeAttribute('aria-activedescendant');
    this.active = -1;
  }
}

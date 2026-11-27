// What every story's browser test shares: where the exported data and the engine's CSV files
// are, reading those files, opening a story, moving the clock, counting the grey levels a globe
// drew, checking that nothing has a hue, and reading a number as the page shows it.
//
// The engine's CSV files are read from results/dashboard/<mode>/ (the export's mode, from the
// manifest), or from SATSIM_RESULTS. With SATSIM_SCREENSHOTS=1 the tests also write the
// report's screenshots to docs/img/phase-5/.

import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { expect, type Page } from '@playwright/test';

export const DATA = resolve('dist/data');
export const manifest = JSON.parse(readFileSync(resolve(DATA, 'manifest.json'), 'utf8')) as {
  settings: { mode: string; window_start_utc: string };
};
export const RESULTS = resolve(process.env.SATSIM_RESULTS ?? `../results/dashboard/${manifest.settings.mode}`);
export const SCREENSHOTS = resolve('../docs/img/phase-5');
export const SHOOT = process.env.SATSIM_SCREENSHOTS === '1';

/** An exported JSON file, as the page loads it. */
export function readData(name: string): unknown {
  return JSON.parse(readFileSync(resolve(DATA, name), 'utf8'));
}

/** A text file the engine wrote. */
export function readResult(name: string): string {
  const path = resolve(RESULTS, name);
  if (!existsSync(path)) {
    throw new Error(`${path} is missing: the browser test compares the page with the engine's files there`);
  }
  return readFileSync(path, 'utf8');
}

/** A CSV file as rows of named fields (quoted fields may hold commas; # lines are comments). */
export function parseCsv(text: string): Record<string, string>[] {
  const lines = text
    .trim()
    .split('\n')
    .filter((line) => !line.startsWith('#'));
  const split = (line: string): string[] => {
    const out: string[] = [];
    let field = '';
    let quoted = false;
    for (const ch of line) {
      if (ch === '"') quoted = !quoted;
      else if (ch === ',' && !quoted) {
        out.push(field);
        field = '';
      } else field += ch;
    }
    out.push(field);
    return out;
  };
  const header = split(lines[0] ?? '');
  return lines.slice(1).map((line) => {
    const cells = split(line);
    return Object.fromEntries(header.map((h, i) => [h, cells[i] ?? '']));
  });
}

/** A CSV file the engine wrote. */
export function readCsv(name: string): Record<string, string>[] {
  return parseCsv(readResult(name));
}

/** A committed file of docs/phase-N-data/, by its path from the repository root. */
export function readCommittedCsv(path: string): Record<string, string>[] {
  return parseCsv(readFileSync(resolve('..', path), 'utf8'));
}

/** Errors on the console or uncaught in the page, collected for the whole test. */
export function watchErrors(page: Page): string[] {
  const errors: string[] = [];
  page.on('console', (m) => {
    if (m.type() === 'error') errors.push(m.text());
  });
  page.on('pageerror', (e) => errors.push(e.message));
  return errors;
}

export async function open(page: Page, story: string): Promise<void> {
  await page.goto(`./#${story}`);
  await expect(page.locator('body')).toHaveAttribute('data-ready', 'true');
  await expect(page.locator('body')).not.toHaveAttribute('data-error', 'true');
}

/** Move the clock by setting the time bar, as a visitor dragging it would. */
export async function seek(page: Page, t: number): Promise<void> {
  await page.locator('.clock-range').evaluate((el, value) => {
    (el as HTMLInputElement).value = String(value);
    el.dispatchEvent(new Event('input'));
  }, t);
  await expect(page.locator('.clock')).toHaveAttribute('data-t', String(t));
}

/** How many distinct colours (on this black-and-white page, grey levels) each of the given page
 * areas has, from one screenshot of the window (decoded in the page; software WebGL makes every
 * screenshot slow). A drawn globe has about 240 to 256 (measured on the small export at
 * 1440x900); the same area with the globe's canvas hidden has 1 or 2, so the specs' "more than
 * 50" still proves that something is drawn. */
export async function colours(page: Page, selectors: readonly string[]): Promise<number[]> {
  const boxes = [];
  for (const s of selectors) boxes.push(await page.locator(s).first().boundingBox());
  const png = (await page.screenshot()).toString('base64');
  return page.evaluate(
    async ({ data, areas }) => {
      const blob = await (await fetch(`data:image/png;base64,${data}`)).blob();
      const bitmap = await createImageBitmap(blob);
      const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
      const ctx = canvas.getContext('2d') as OffscreenCanvasRenderingContext2D;
      ctx.drawImage(bitmap, 0, 0);
      return areas.map((box) => {
        if (box === null || box.width < 1 || box.height < 1) return 0;
        const pixels = ctx.getImageData(box.x, box.y, box.width, box.height).data;
        const seen = new Set<number>();
        for (let i = 0; i < pixels.length; i += 4) {
          seen.add(((pixels[i] ?? 0) << 16) | ((pixels[i + 1] ?? 0) << 8) | (pixels[i + 2] ?? 0));
        }
        return seen.size;
      });
    },
    { data: png, areas: boxes },
  );
}

/** The page has no colour with a hue: in a screenshot (of the whole page, or of the window)
 * every pixel's red, green and blue differ by at most `tolerance` (the page is drawn in black,
 * white and greys only). */
export async function expectNoHue(page: Page, tolerance = 3, fullPage = true): Promise<void> {
  const png = (await page.screenshot({ fullPage })).toString('base64');
  const found = await page.evaluate(
    async ({ data, tol }) => {
      const blob = await (await fetch(`data:image/png;base64,${data}`)).blob();
      const bitmap = await createImageBitmap(blob);
      const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
      const ctx = canvas.getContext('2d') as OffscreenCanvasRenderingContext2D;
      ctx.drawImage(bitmap, 0, 0);
      const pixels = ctx.getImageData(0, 0, bitmap.width, bitmap.height).data;
      let count = 0;
      let worst = 0;
      let at = '';
      for (let i = 0; i < pixels.length; i += 4) {
        const r = pixels[i] ?? 0;
        const g = pixels[i + 1] ?? 0;
        const b = pixels[i + 2] ?? 0;
        const hue = Math.max(Math.abs(r - g), Math.abs(g - b), Math.abs(r - b));
        if (hue > tol) {
          count++;
          if (hue > worst) {
            worst = hue;
            const p = i / 4;
            at = `(${p % bitmap.width}, ${Math.floor(p / bitmap.width)}) rgb(${r}, ${g}, ${b})`;
          }
        }
      }
      return { count, worst, at };
    },
    { data: png, tol: tolerance },
  );
  expect(found.count, `pixels with a hue; the strongest at ${found.at}`).toBe(0);
}

/** A number as shown ("100 ms", "8 s", "186,816", "94.4%") in the unit of the engine's value,
 * and the rounding the display allows (half a unit of its last digit). */
export function shown(text: string, unit: 's' | 'ms' | 'count' | 'share'): { value: number; tolerance: number } {
  const match = /^(-?[\d,]+(?:\.(\d+))?)(?: ?(ms|s|%))?$/.exec(text.trim());
  if (match === null) throw new Error(`"${text}" is not a number the page should show`);
  const value = Number((match[1] ?? '').replaceAll(',', ''));
  const decimals = match[2]?.length ?? 0;
  const shownUnit = match[3] ?? 'count';
  const scale =
    shownUnit === '%' ? 0.01 : shownUnit === unit || shownUnit === 'count' ? 1 : shownUnit === 'ms' ? 0.001 : 1000;
  return { value: value * scale, tolerance: 0.5 * 10 ** -decimals * scale };
}

/** The page shows `text` for the engine's value `engine` (a CSV or summary text), rounded only. */
export function expectShown(text: string, engine: string | number, unit: 's' | 'ms' | 'count' | 'share', what: string): void {
  const { value, tolerance } = shown(text, unit);
  expect(Math.abs(value - Number(engine)), `${what}: page "${text}", engine ${engine}`).toBeLessThanOrEqual(tolerance * (1 + 1e-9));
}

/** The UTC text of `seconds` after a UTC text (both YYYY-MM-DDTHH:MM:SS, UTC). */
export function utcPlus(utc: string, seconds: number): string {
  return new Date(Date.parse(`${utc}Z`) + seconds * 1000).toISOString().slice(0, 19);
}

/** The clock bar covers none of the given globe areas: checked with each globe scrolled to the
 * top of the window and to its bottom, where a bar stuck to the window's edge would lie. */
export async function expectClockClear(page: Page, globes: readonly string[]): Promise<void> {
  for (const selector of globes) {
    const globe = page.locator(selector).first();
    for (const block of ['start', 'end'] as const) {
      await globe.evaluate((el, b) => el.scrollIntoView({ block: b }), block);
      const g = await globe.boundingBox();
      const c = await page.locator('.clock').boundingBox();
      expect(g, `${selector} is on the page`).not.toBeNull();
      expect(c, 'the clock bar is on the page').not.toBeNull();
      if (g === null || c === null) return;
      const overlap = c.x < g.x + g.width && g.x < c.x + c.width && c.y < g.y + g.height && g.y < c.y + c.height;
      expect(overlap, `the clock bar (${JSON.stringify(c)}) lies over ${selector} (${JSON.stringify(g)})`).toBe(false);
    }
  }
}

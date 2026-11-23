// The browser test of the failure story (spec 6.3): it loads without a console error, the
// globe draws something, moving the clock changes the state the page reports, and the numbers
// on screen equal the engine's CSV files, which this test reads itself.
//
// The engine's CSV files are read from results/dashboard/<mode>/ (the export's mode, from the
// manifest), or from SATSIM_RESULTS. With SATSIM_SCREENSHOTS=1 the test also writes the
// report's screenshots to docs/img/phase-5/.

import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { expect, test, type Page } from '@playwright/test';

const DATA = resolve('dist/data');
const manifest = JSON.parse(readFileSync(resolve(DATA, 'manifest.json'), 'utf8')) as { settings: { mode: string } };
const RESULTS = resolve(process.env.SATSIM_RESULTS ?? `../results/dashboard/${manifest.settings.mode}`);
const SCREENSHOTS = resolve('../docs/img/phase-5');
const MODES = ['planner', 'local-repair', 'flooding'] as const;

interface StoryFile {
  scenarios: { scenario: string; failure_s: number; sources: { events: string; measures: string } }[];
}
const story = JSON.parse(readFileSync(resolve(DATA, 'story-failure.json'), 'utf8')) as StoryFile;
const first = story.scenarios[0];
if (first === undefined) throw new Error('story-failure.json has no scenario');

/** A CSV file as rows of named fields (quoted fields may hold commas). */
function readCsv(name: string): Record<string, string>[] {
  const path = resolve(RESULTS, name);
  if (!existsSync(path)) {
    throw new Error(`${path} is missing: the browser test compares the page with the engine's CSV files there`);
  }
  const lines = readFileSync(path, 'utf8').trim().split('\n');
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

/** Errors on the console or uncaught in the page, collected for the whole test. */
function watchErrors(page: Page): string[] {
  const errors: string[] = [];
  page.on('console', (m) => {
    if (m.type() === 'error') errors.push(m.text());
  });
  page.on('pageerror', (e) => errors.push(e.message));
  return errors;
}

async function open(page: Page): Promise<void> {
  await page.goto('./#failure');
  await expect(page.locator('body')).toHaveAttribute('data-ready', 'true');
  await expect(page.locator('body')).not.toHaveAttribute('data-error', 'true');
}

async function seek(page: Page, t: number): Promise<void> {
  await page.locator('.clock-range').evaluate((el, value) => {
    (el as HTMLInputElement).value = String(value);
    el.dispatchEvent(new Event('input'));
  }, t);
  await expect(page.locator('.clock')).toHaveAttribute('data-t', String(t));
}

/** How many distinct colours each of the given page areas has, from one screenshot of the
 * window (decoded in the page; software WebGL makes every screenshot slow). */
async function colours(page: Page, selectors: readonly string[]): Promise<number[]> {
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

/** A number as shown ("100 ms", "8 s", "186,816") in the unit of the CSV column, and the
 * rounding the display allows (half a unit of its last digit). */
function shown(text: string, csvUnit: 's' | 'ms' | 'count'): { value: number; tolerance: number } {
  const match = /^(-?[\d,]+(?:\.(\d+))?)(?: (ms|s))?$/.exec(text.trim());
  if (match === null) throw new Error(`"${text}" is not a number the page should show`);
  const value = Number((match[1] ?? '').replaceAll(',', ''));
  const decimals = match[2]?.length ?? 0;
  const unit = match[3] ?? 'count';
  const scale = unit === csvUnit || unit === 'count' ? 1 : unit === 'ms' ? 0.001 : 1000;
  return { value: value * scale, tolerance: 0.5 * 10 ** -decimals * scale };
}

test('the failure story loads, draws, follows the clock and shows the engine numbers', async ({ page }) => {
  const errors = watchErrors(page);
  await open(page);
  await expect(page.locator('.story')).toHaveAttribute('data-scenario', first.scenario);

  // The globe drew something: each lane's view is not one flat colour.
  await page.locator('.lanes').scrollIntoViewIfNeeded();
  const counts = await colours(page, MODES.map((mode) => `.lane[data-mode="${mode}"] .lane-globe`));
  for (const count of counts) expect(count).toBeGreaterThan(50);

  // Moving the clock changes the state each lane reports, to the engine's state at that time.
  const events = readCsv(first.sources.events);
  const stateAt = (mode: string, t: number): string | undefined =>
    events.find((r) => r.reaction === mode && Number(r.start_s) <= t && t < Number(r.end_s))?.state;
  const times = [first.failure_s - 1, first.failure_s + 0.05, first.failure_s + 3];
  const seen = new Set<string>();
  for (const t of times) {
    await seek(page, t);
    for (const mode of MODES) {
      const expected = stateAt(mode, t);
      expect(expected, `the events CSV has a ${mode} row at ${t} s`).toBeDefined();
      await expect(page.locator(`.lane[data-mode="${mode}"]`)).toHaveAttribute('data-state', expected ?? '');
      seen.add(`${mode}:${expected ?? ''}`);
    }
  }
  expect(seen.has('planner:delivered') && seen.has('planner:black_hole')).toBe(true);

  // The facts list follows the clock too.
  await seek(page, first.failure_s - 1);
  await expect(page.locator('.facts tr[data-fact]').first()).toHaveAttribute('data-happened', 'false');
  await seek(page, first.failure_s + 3);
  await expect(page.locator('.facts tr[data-fact]').first()).toHaveAttribute('data-happened', 'true');

  // The measures panel shows exactly the run's measures CSV, rounded only for display.
  const measures = readCsv(first.sources.measures);
  const columns: [string, 's' | 'ms' | 'count'][] = [
    ['lost_s', 's'],
    ['black_hole_s', 's'],
    ['loop_s', 's'],
    ['restore_s', 's'],
    ['settle_s', 's'],
    ['extra_median_ms', 'ms'],
    ['extra_max_ms', 'ms'],
    ['extra_settled_ms', 'ms'],
    ['reports', 'count'],
    ['messages', 'count'],
  ];
  for (const mode of MODES) {
    const row = measures.find((r) => r.reaction === mode);
    expect(row, `the measures CSV has a ${mode} row`).toBeDefined();
    for (const [column, unit] of columns) {
      const csv = row?.[column] ?? '';
      const text = (await page.locator(`.measures td[data-mode="${mode}"][data-measure="${column}"]`).textContent()) ?? '';
      if (csv === 'none') {
        expect(text, `${mode} ${column}`).toBe('none');
        continue;
      }
      const { value, tolerance } = shown(text, unit);
      expect(Math.abs(value - Number(csv)), `${mode} ${column}: page "${text}", CSV ${csv}`).toBeLessThanOrEqual(
        tolerance * (1 + 1e-9),
      );
    }
  }

  expect(errors).toEqual([]);
});

test('a slow-motion span is labelled with the speed it plays at', async ({ page }) => {
  await open(page);
  await seek(page, first.failure_s + 0.5);
  await expect(page.locator('.clock-status')).toHaveText('slow motion 0.02x');
  await expect(page.locator('.clock')).toHaveAttribute('data-slow-motion', 'true');
});

test('a file that is not the size the manifest gives is reported and nothing is drawn', async ({ page }) => {
  await page.route('**/data/story-failure.json', async (route) => {
    const response = await route.fetch();
    const body = await response.body();
    await route.fulfill({ response, body: body.subarray(0, body.length - 1) });
  });
  await page.goto('./#failure');
  await expect(page.locator('body')).toHaveAttribute('data-error', 'true');
  await expect(page.locator('.status.error')).toContainText('The file story-failure.json could not be used');
  await expect(page.locator('.lanes')).toHaveCount(0);
});

test.describe('screenshots for the report', () => {
  test.skip(process.env.SATSIM_SCREENSHOTS !== '1', 'set SATSIM_SCREENSHOTS=1 to write them');
  test.use({ deviceScaleFactor: 2 });

  test('wide and phone', async ({ page }) => {
    mkdirSync(SCREENSHOTS, { recursive: true });
    await open(page);
    await seek(page, first.failure_s + 0.15);
    await page.waitForTimeout(500);
    await page.screenshot({ path: resolve(SCREENSHOTS, 'failure-story.png'), fullPage: true });
    await page.locator('.lanes').screenshot({ path: resolve(SCREENSHOTS, 'failure-lanes.png') });
    // On a phone the lanes are stacked; only the lanes in view are drawn, so the shot is of the
    // window with the lanes scrolled into it.
    await page.setViewportSize({ width: 390, height: 844 });
    await page.locator('.lane[data-mode="planner"]').evaluate((el) => el.scrollIntoView({ block: 'start' }));
    await page.waitForTimeout(500);
    await page.screenshot({ path: resolve(SCREENSHOTS, 'failure-story-phone.png') });
  });
});

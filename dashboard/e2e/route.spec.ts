// The browser test of the route story (spec 6.3): it loads without a console error, the globe
// draws something, moving the clock changes the route each strategy reports, to the engine's
// route or drop at that second (routes CSV), and the table equals the routing summary.

import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { expect, test, type Page } from '@playwright/test';
import { colours, expectClockClear, expectShown, open, readCsv, readData, readResult, SCREENSHOTS, seek, SHOOT, utcPlus, watchErrors } from './helpers';

interface StoryFile {
  sources: { routes: string; summary: string };
  settings: { start_utc: string; duration_s: number; strategies: string[] };
  directions: { from: number; to: number }[];
}
const story = readData('story-route.json') as StoryFile;
const sites = (readData('sites.json') as { sites: { name: string }[] }).sites;
const name = (i: number): string => sites[i]?.name ?? '';
const first = story.directions[0];
if (first === undefined) throw new Error('story-route.json has no direction');
const STRATEGIES = story.settings.strategies;

/** The routes CSV rows of one direction, by strategy and UTC. */
function routesOf(from: string, to: string): Map<string, Record<string, string>> {
  const rows = new Map<string, Record<string, string>>();
  for (const r of readCsv(story.sources.routes)) {
    if (r.from === from && r.to === to) rows.set(`${r.strategy} ${r.utc}`, r);
  }
  return rows;
}

/** One strategy's numbers in the routing summary's block for a direction. */
function summaryOf(from: string, to: string, strategy: string): { median: string; changes: number; drops: string; noRoute: number } {
  const lines = readResult(story.sources.summary).split('\n');
  const start = lines.findIndex((l) => l.trim().startsWith(`${from} -> ${to} (`));
  expect(start, `the summary has a block for ${from} -> ${to}`).toBeGreaterThanOrEqual(0);
  const own = lines.findIndex((l, i) => i > start && l.trim().startsWith(`${strategy}: ok `));
  const block = lines.slice(own, own + 9).join('\n');
  const median = /delay: min [\d.]+ ms, p5 [\d.]+ ms, median ([\d.]+) ms/.exec(block)?.[1] ?? 'none';
  const counts = /counts: bad (\d+), increases (\d+)/.exec(block);
  const noRoute = /no route: ([^;]+); drop events (\d+)/.exec(block);
  let seconds = 0;
  if (noRoute?.[1] !== 'none') for (const m of (noRoute?.[1] ?? '').matchAll(/ ([\d.]+) s/g)) seconds += Number(m[1]);
  return {
    median,
    changes: Number(counts?.[1]) + Number(counts?.[2]),
    drops: noRoute?.[2] ?? '',
    noRoute: seconds,
  };
}

async function checkStatesAt(page: Page, t: number, rows: Map<string, Record<string, string>>): Promise<string[]> {
  await seek(page, t);
  const seen: string[] = [];
  const utc = utcPlus(story.settings.start_utc, Math.floor(t));
  for (const s of STRATEGIES) {
    const row = rows.get(`${s} ${utc}`);
    expect(row, `the routes CSV has ${s} at ${utc}`).toBeDefined();
    const li = page.locator(`.readouts li[data-strategy="${s}"]`);
    if (row?.status === 'ok') {
      await expect(li).toHaveAttribute('data-state', 'route');
      await expect(li).toHaveAttribute('data-route', row.route ?? '');
      seen.push(`${s}:route`);
    } else {
      await expect(li).toHaveAttribute('data-state', 'drop');
      await expect(li).toHaveAttribute('data-cause', row?.drop_cause ?? '');
      seen.push(`${s}:drop`);
    }
  }
  return seen;
}

test('the route story loads, draws, follows the clock and shows the engine numbers', async ({ page }) => {
  const errors = watchErrors(page);
  await open(page, 'route');
  const from = name(first.from);
  const to = name(first.to);
  await expect(page.locator('.story')).toHaveAttribute('data-from', from);

  await page.locator('.stage').scrollIntoViewIfNeeded();
  const [count] = await colours(page, ['.stage-globe']);
  expect(count).toBeGreaterThan(50);

  // The clock's time picks each strategy's route or drop, as the routes CSV has it at that second:
  // at the start, the first second of the first lowest-delay drop, the second before it, the end.
  const rows = routesOf(from, to);
  const firstDrop = [...rows.values()].find((r) => r.strategy === STRATEGIES[0] && r.status !== 'ok');
  expect(firstDrop, 'the run has a drop').toBeDefined();
  const dropAt = (Date.parse(`${firstDrop?.utc ?? ''}Z`) - Date.parse(`${story.settings.start_utc}Z`)) / 1000;
  const seen = new Set<string>();
  for (const t of [0, dropAt - 0.5, dropAt, dropAt + 0.5, story.settings.duration_s]) {
    for (const s of await checkStatesAt(page, t, rows)) seen.add(s);
  }
  expect(seen.has(`${STRATEGIES[0] ?? ''}:route`) && seen.has(`${STRATEGIES[0] ?? ''}:drop`)).toBe(true);

  // The table equals the routing summary, column by column.
  for (const s of STRATEGIES) {
    const sum = summaryOf(from, to, s);
    const cell = async (measure: string): Promise<string> =>
      (await page.locator(`.summary-table td[data-strategy="${s}"][data-measure="${measure}"]`).textContent()) ?? '';
    if (sum.median === 'none') expect(await cell('delay_median_ms')).toBe('none');
    else expectShown(await cell('delay_median_ms'), sum.median, 'ms', `${s} median delay`);
    expectShown(await cell('changes'), sum.changes, 'count', `${s} route changes`);
    expectShown(await cell('drop_events'), sum.drops, 'count', `${s} drops`);
    expectShown(await cell('no_route_total_s'), sum.noRoute, 's', `${s} time without a route`);
  }

  // The other direction: the page follows the menu, and its routes are the other direction's.
  if (story.directions.some((d) => d.from === first.to && d.to === first.from)) {
    await page.locator('.segmented[aria-label="Direction"] button').nth(1).click();
    await expect(page.locator('.story')).toHaveAttribute('data-from', to);
    await checkStatesAt(page, 0, routesOf(to, from));
  }

  expect(errors).toEqual([]);
});

test('a strategy switched off is neither drawn nor charted', async ({ page }) => {
  await open(page, 'route');
  const s = STRATEGIES[1] ?? '';
  await page.locator(`.toggle[data-value="${s}"]`).click();
  await expect(page.locator(`.readouts li[data-strategy="${s}"]`)).toHaveAttribute('data-on', 'false');
  await expect(page.locator(`.delay-row[data-strategy="${s}"]`)).toBeHidden();
  await expect(page.locator(`.delay-row[data-strategy="${STRATEGIES[0] ?? ''}"]`)).toBeVisible();
});

test('the clock bar lies over no globe, wide and on a phone', async ({ page }) => {
  await open(page, 'route');
  await expectClockClear(page, ['.stage']);
  await page.setViewportSize({ width: 390, height: 844 });
  await expectClockClear(page, ['.stage']);
});

test.describe('screenshot for the report', () => {
  test.skip(!SHOOT, 'set SATSIM_SCREENSHOTS=1 to write it');
  test.use({ deviceScaleFactor: 2 });
  test('wide', async ({ page }) => {
    mkdirSync(SCREENSHOTS, { recursive: true });
    await open(page, 'route');
    await seek(page, Math.min(50, story.settings.duration_s));
    await page.locator('.control-bar').evaluate((el) => el.scrollIntoView({ block: 'start' }));
    await page.waitForTimeout(500);
    await page.screenshot({ path: resolve(SCREENSHOTS, 'route-story.png') });
  });
});

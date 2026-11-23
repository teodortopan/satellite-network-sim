// The browser test of the resilience story (spec 6.3): it loads without a console error, the
// globe draws something, the failed-share menu changes the state the page reports, each study
// pair's route after recovery equals the one-step run's routes CSV, the network table its
// network CSV, and the numbers behind the charts the committed Phase 4 files.

import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { expect, test } from '@playwright/test';
import { colours, expectShown, open, readCommittedCsv, readCsv, readData, SCREENSHOTS, SHOOT, watchErrors } from './helpers';

interface StoryFile {
  charts: Record<string, { source: string }>;
  steps: { name: string; sources: { routes: string; network: string; failures: string; links: string } }[];
}
const story = readData('story-resilience.json') as StoryFile;

test('the resilience story loads, draws, follows its menu and shows the engine numbers', async ({ page }) => {
  const errors = watchErrors(page);
  await open(page, 'resilience');
  await page.locator('.instant .stage').scrollIntoViewIfNeeded();
  const [count] = await colours(page, ['.instant .stage-globe']);
  expect(count).toBeGreaterThan(50);

  const buttons = page.locator('.segmented[aria-label="Failed share"] button');
  await expect(buttons).toHaveCount(story.steps.length);
  for (const [i, step] of story.steps.entries()) {
    await buttons.nth(i).click();
    await expect(page.locator('.story')).toHaveAttribute('data-step', step.name);

    // The failed satellites, and the planned links that did not fail: the run's link plan
    // without every link of a failed satellite.
    const failed = new Set(readCsv(step.sources.failures).filter((r) => r.kind === 'satellite').map((r) => r.norad_cat_id_a));
    const globe = page.locator('.instant .stage-globe');
    await expect(globe).toHaveAttribute('data-failed-satellites', String(failed.size));
    const kept = readCsv(step.sources.links).filter((r) => !failed.has(r.norad_cat_id_a) && !failed.has(r.norad_cat_id_b));
    await expect(globe).toHaveAttribute('data-links-drawn', String(kept.length));

    // Each pair's route after recovery (the reference lane), or no route.
    const routes = readCsv(step.sources.routes).filter((r) => r.reaction === 'reference');
    const items = page.locator('.pair-list li');
    await expect(items).toHaveCount(routes.length);
    for (const [k, row] of routes.entries()) {
      const li = items.nth(k);
      await expect(li).toHaveAttribute('data-from', row.from ?? '');
      await expect(li).toHaveAttribute('data-to', row.to ?? '');
      await expect(li).toHaveAttribute('data-state', row.state ?? '');
      const routed = row.state === 'delivered';
      await expect(li).toHaveAttribute('data-routed', String(routed));
      await expect(li).toHaveAttribute('data-route', routed ? (row.route ?? '') : '');
      if (routed) {
        expect(Number(await li.getAttribute('data-one-way-ms'))).toBe(Number(row.one_way_ms));
        expectShown(((await li.locator('.pair-state').textContent()) ?? '').split(' over ')[0] ?? '', row.one_way_ms ?? '', 'ms', `${row.from ?? ''} delay`);
      } else {
        await expect(li.locator('.pair-state')).toHaveText(/^no route/);
      }
    }

    // The pieces of the network, as the run's network CSV has them.
    for (const row of readCsv(step.sources.network)) {
      const cell = async (field: string): Promise<string> =>
        (await page.locator(`.network-panel td[data-scope="${row.scope ?? ''}"][data-field="${field}"]`).textContent()) ?? '';
      expectShown(await cell('working_satellites'), row.working_satellites ?? '', 'count', `${row.scope ?? ''} working`);
      expectShown(await cell('pieces'), row.pieces ?? '', 'count', `${row.scope ?? ''} pieces`);
      expectShown(await cell('largest_piece'), row.largest_piece ?? '', 'count', `${row.scope ?? ''} largest piece`);
      expectShown(await cell('largest_piece_share'), row.largest_piece_share ?? '', 'share', `${row.scope ?? ''} largest share`);
    }
  }

  // The numbers behind the charts are the committed files' numbers, row by row.
  for (const name of ['recovery_summary', 'percolation']) {
    const rows = readCommittedCsv(story.charts[name]?.source ?? '');
    const table = page.locator(`table[data-source="${name}"]`);
    await expect(table.locator('tbody tr')).toHaveCount(rows.length);
    const cells = await table.locator('tbody tr').evaluateAll((trs) =>
      trs.map((tr) => Object.fromEntries([...tr.querySelectorAll('td')].map((td) => [td.dataset.column ?? '', td.textContent]))),
    );
    rows.forEach((row, i) => {
      for (const [column, text] of Object.entries(cells[i] ?? {})) {
        const csv = row[column] ?? '';
        if (csv !== '' && !Number.isNaN(Number(csv))) expect(Number(text), `${name} row ${i} ${column}`).toBe(Number(csv));
        else expect(text, `${name} row ${i} ${column}`).toBe(csv);
      }
    });
  }

  expect(errors).toEqual([]);
});

test.describe('screenshot for the report', () => {
  test.skip(!SHOOT, 'set SATSIM_SCREENSHOTS=1 to write it');
  test.use({ deviceScaleFactor: 2 });
  test('wide', async ({ page }) => {
    mkdirSync(SCREENSHOTS, { recursive: true });
    await open(page, 'resilience');
    await page.locator('.panels.charts').evaluate((el) => el.scrollIntoView({ block: 'start' }));
    await page.waitForTimeout(500);
    await page.screenshot({ path: resolve(SCREENSHOTS, 'resilience-story.png') });
  });
});

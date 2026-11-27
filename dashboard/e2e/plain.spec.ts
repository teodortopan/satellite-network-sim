// Every story can be understood by a visitor who has never seen the project: a "What to try"
// line, a legend right beside its globe, the longer explanation folded under "How to read
// this", the files and settings under "Details and sources", and the globe with its controls
// on the first screen of a 1440x900 window. The menu names each story with a plain summary.

import { expect, test } from '@playwright/test';
import { open } from './helpers';

const GLOBES: Record<string, string> = {
  explore: '.stage-globe',
  failure: '.lanes',
  route: '.stage-globe',
  constellation: '.stage-globe',
  resilience: '.instant .stage-globe',
};

/** How far apart two boxes are (0 when they touch or overlap), in CSS pixels. */
function gap(a: { x: number; y: number; width: number; height: number }, b: typeof a): number {
  const dx = Math.max(0, b.x - (a.x + a.width), a.x - (b.x + b.width));
  const dy = Math.max(0, b.y - (a.y + a.height), a.y - (b.y + b.height));
  return Math.hypot(dx, dy);
}

for (const [story, globe] of Object.entries(GLOBES)) {
  test(`the ${story} story says what to try, has a legend by its globe and folds its details`, async ({ page }) => {
    await open(page, story);
    const intro = page.locator('.story .intro');
    await expect(intro.locator('h1')).toBeVisible();
    await expect(intro.locator('.lede')).not.toBeEmpty();
    await expect(intro.locator('.try')).toBeVisible();
    await expect(intro.locator('.try')).toContainText('What to try:');
    await expect(intro.locator('.try-text')).not.toBeEmpty();

    // The folds: closed at first, opened by their summary, and not empty.
    for (const [fold, title] of [
      ['how-to-read', 'How to read this'],
      ['sources', 'Details and sources'],
    ] as const) {
      const details = intro.locator(`details.${fold}`);
      await expect(details).toHaveCount(1);
      await expect(details.locator('summary')).toHaveText(title);
      await expect(details).not.toHaveAttribute('open');
      await details.locator('summary').click();
      await expect(details).toHaveAttribute('open');
      await expect(details.locator('.fold-body')).toBeVisible();
      expect(((await details.locator('.fold-body').textContent()) ?? '').trim().length).toBeGreaterThan(40);
      await details.locator('summary').click();
    }

    // A legend with at least three marks, each with a swatch, right beside or above the globe.
    const legend = page.locator('.globe-legend').first();
    await expect(legend).toBeVisible();
    expect(await legend.locator('li').count()).toBeGreaterThanOrEqual(3);
    expect(await legend.locator('li .swatch').count()).toBe(await legend.locator('li').count());
    const g = await page.locator(globe).first().boundingBox();
    const l = await legend.boundingBox();
    expect(g !== null && l !== null && gap(g, l) < 120, `the legend lies by the globe: ${JSON.stringify({ g, l })}`).toBe(true);
  });

  test(`the ${story} story shows its globe and controls on the first screen`, async ({ page }) => {
    await open(page, story);
    const height = page.viewportSize()?.height ?? 0;
    const g = await page.locator(globe).first().boundingBox();
    expect(g, 'the globe is on the page').not.toBeNull();
    // At least half of the globe is in the first screen.
    expect((g?.y ?? height) + (g?.height ?? 0) / 2).toBeLessThanOrEqual(height);
    for (const control of ['.clock-play', '.control-bar, .scenario-bar']) {
      const c = page.locator(control).first();
      if ((await c.count()) === 0) continue;
      const box = await c.boundingBox();
      expect((box?.y ?? height) + (box?.height ?? 0), `${control} is in the first screen`).toBeLessThanOrEqual(height);
    }
  });
}

test('the menu lists "Try it" first, then the saved runs, each with a plain one-line summary', async ({ page }) => {
  await open(page, 'route');
  const menu = page.locator('#story-menu');
  await expect(menu.locator('.menu-label')).toHaveText('Saved runs');
  await expect(menu.locator('.menu-primary a[data-story]')).toHaveCount(1);
  await expect(menu.locator('a[data-story]').first()).toHaveAttribute('data-story', 'explore');
  await expect(menu.locator('a[data-story]').first().locator('.menu-title')).toHaveText('Try it');
  const links = menu.locator('a[data-story]');
  expect(await links.count()).toBe(Object.keys(GLOBES).length);
  for (const link of await links.all()) {
    const summary = (await link.getAttribute('title')) ?? '';
    expect(summary.length).toBeGreaterThan(20);
    await expect(link.locator('.menu-summary')).toHaveText(summary);
  }
  await expect(menu.locator('a[aria-current="page"]')).toHaveAttribute('data-story', 'route');
});

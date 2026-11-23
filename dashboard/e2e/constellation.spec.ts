// The browser test of the constellation story (spec 6.3): it loads without a console error,
// the globe draws something, the shell and pattern menus change what is drawn (read back from
// the drawn objects), and the tables equal the constellation and topology programs' output.

import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { expect, test } from '@playwright/test';
import { colours, expectClockClear, expectShown, open, readCsv, readData, readResult, SCREENSHOTS, SHOOT, watchErrors } from './helpers';

interface StoryFile {
  sources: { shells: string };
  patterns: { pattern: string }[];
}
const story = readData('story-constellation.json') as StoryFile;
const PATTERNS = story.patterns.map((p) => p.pattern);
const satellites = readCsv('constellation.csv').filter((r) => r.plane_index !== '');
const shellOf = new Map(satellites.map((r) => [r.norad_cat_id ?? '', r.shell ?? '']));
const SHELLS = [...new Set(satellites.map((r) => r.shell ?? ''))];

/** The planned links of a pattern (links-<pattern>.csv), optionally only those inside a shell. */
function linkCount(pattern: string, shell: string | null): number {
  return readCsv(`links-${pattern}.csv`).filter(
    (r) => shell === null || (shellOf.get(r.norad_cat_id_a ?? '') === shell && shellOf.get(r.norad_cat_id_b ?? '') === shell),
  ).length;
}

test('the constellation story loads, draws and follows its menus', async ({ page }) => {
  const errors = watchErrors(page);
  await open(page, 'constellation');
  const globe = page.locator('.stage-globe');
  await page.locator('.stage').scrollIntoViewIfNeeded();
  const [count] = await colours(page, ['.stage-globe']);
  expect(count).toBeGreaterThan(50);

  // Every satellite in a plane is chosen, then one shell's.
  await expect(globe).toHaveAttribute('data-satellites-chosen', String(satellites.length));
  const shellMenu = page.locator('.dropdown', { hasText: 'Shell' }).locator('select');
  const last = SHELLS[SHELLS.length - 1] ?? '';
  await shellMenu.selectOption(last);
  await expect(page.locator('.story')).toHaveAttribute('data-shell', last);
  await expect(globe).toHaveAttribute('data-satellites-chosen', String(satellites.filter((r) => r.shell === last).length));

  // One plane of that shell: its satellites in the constellation output.
  await page.locator('.dropdown', { hasText: 'Plane' }).locator('select').selectOption('1');
  await expect(page.locator('.story')).toHaveAttribute('data-plane', '1');
  await expect(globe).toHaveAttribute(
    'data-satellites-chosen',
    String(satellites.filter((r) => r.shell === last && r.plane_index === '1').length),
  );

  // Each pattern draws its own planned links: within the shell, then over all shells.
  await shellMenu.selectOption(last);
  for (const pattern of PATTERNS) {
    await page.locator(`.segmented[aria-label="Laser pattern"] button[data-value="${pattern}"]`).click();
    await expect(globe).toHaveAttribute('data-drawn-pattern', pattern);
    await expect(globe).toHaveAttribute('data-links-drawn', String(linkCount(pattern, last)));
  }
  await shellMenu.selectOption('all');
  for (const pattern of PATTERNS) {
    await page.locator(`.segmented[aria-label="Laser pattern"] button[data-value="${pattern}"]`).click();
    await expect(globe).toHaveAttribute('data-drawn-pattern', pattern);
    await expect(globe).toHaveAttribute('data-links-drawn', String(linkCount(pattern, null)));
  }

  // Ground sites switch by kind.
  await page.locator('.toggle[data-value="gateway"]').click();
  await expect(globe).toHaveAttribute('data-sites', 'city pop');

  // The shells table equals the constellation summary.
  const summary = readResult(story.sources.shells);
  for (const shell of SHELLS) {
    const m = new RegExp(
      `shell ${shell} \\(inclination ([\\d.]+) \\+- ([\\d.]+) deg, height ([\\d.]+) to ([\\d.]+) km.*: (\\d+) satellites in the bands, (\\d+) in planes, (\\d+) planes`,
    ).exec(summary);
    expect(m, `the summary has shell ${shell}`).not.toBeNull();
    const cell = async (field: string): Promise<string> =>
      (await page.locator(`.shells-panel td[data-shell="${shell}"][data-field="${field}"]`).textContent()) ?? '';
    expectShown(await cell('satellites_in_planes'), m?.[6] ?? '', 'count', `${shell} satellites`);
    expectShown(await cell('planes'), m?.[7] ?? '', 'count', `${shell} planes`);
    const [lo, hi] = (await cell('height')).replace(' km', '').split(' to ');
    expect(Number(lo)).toBe(Number(m?.[3]));
    expect(Number(hi)).toBe(Number(m?.[4]));
    const [inc, tol] = (await cell('inclination')).replaceAll('°', '').split(' ± ');
    expect(Number(inc)).toBe(Number(m?.[1]));
    expect(Number(tol)).toBe(Number(m?.[2]));
  }

  // The patterns table equals the topology summaries.
  for (const pattern of PATTERNS) {
    const text = readResult(`topology-${pattern}.summary.txt`);
    for (const [field, key] of [
      ['links', 'planned laser links'],
      ['in_plane', 'in_plane'],
      ['cross_plane', 'cross_plane'],
      ['second_cross_plane', 'second_cross_plane'],
    ] as const) {
      const value = new RegExp(`^\\s+${key}: (\\d+)$`, 'm').exec(text)?.[1] ?? '';
      const shown = (await page.locator(`.patterns-panel td[data-pattern="${pattern}"][data-field="${field}"]`).textContent()) ?? '';
      expectShown(shown, value, 'count', `${pattern} ${field}`);
    }
  }

  expect(errors).toEqual([]);
});

test('the clock bar lies over no globe, wide and on a phone', async ({ page }) => {
  await open(page, 'constellation');
  await expectClockClear(page, ['.stage']);
  await page.setViewportSize({ width: 390, height: 844 });
  await expectClockClear(page, ['.stage']);
});

test.describe('screenshot for the report', () => {
  test.skip(!SHOOT, 'set SATSIM_SCREENSHOTS=1 to write it');
  test.use({ deviceScaleFactor: 2 });
  test('wide', async ({ page }) => {
    mkdirSync(SCREENSHOTS, { recursive: true });
    await open(page, 'constellation');
    await page.locator('.control-bar').evaluate((el) => el.scrollIntoView({ block: 'start' }));
    await page.waitForTimeout(500);
    await page.screenshot({ path: resolve(SCREENSHOTS, 'constellation-story.png') });
  });
});

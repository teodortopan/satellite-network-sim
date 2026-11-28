// The browser test of "Try it", the landing page: it opens with the saved run playing; a run
// chosen by keyboard is worked out in the page by the WebAssembly engine, and what the page
// shows (each rule's route or drop at a few clock times, the headline) equals the routes CSV
// the engine returned; a click on the globe sets a place where the camera looks; a shared link
// reproduces a run; impossible pairs and places without coverage are said plainly; the page
// keeps animating while the engine works.
//
// The engine's own output is read through window.__satsimExplore and the section's data-run
// (a digest of the routes CSV), which the page keeps for this test (stories/explore/index.ts).

import { expect, test, type Page } from '@playwright/test';
import { open, parseCsv, readData, seekReadouts, utcPlus, watchErrors } from './helpers';

interface EngineBlock {
  strategies: string[];
  window: { start_utc: string; duration_s: number };
}
const engine = (readData('manifest.json') as { engine: EngineBlock }).engine;
const DURATION = Math.min(600, engine.window.duration_s);
const sites = (readData('sites.json') as { sites: { name: string; lat_deg: number; lon_deg: number }[] }).sites;
const story = readData('story-route.json') as { directions: { from: number; to: number }[] };
const RUN_TIMEOUT = 120_000;

interface LastRun {
  routesCsv: string;
  dropsCsv: string;
  exitCode: number;
  names: { from: string; to: string };
  request: { start_utc: string; duration_s: number; from: { name: string }; to: { name: string } };
}

/** The same digest as the page's (FNV-1a, 32 bits). */
function digest(text: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < text.length; i++) {
    h ^= text.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return (h >>> 0).toString(16).padStart(8, '0');
}

async function lastRun(page: Page): Promise<LastRun> {
  const last = await page.evaluate(() => (window as unknown as { __satsimExplore?: { last: unknown } }).__satsimExplore?.last ?? null);
  expect(last, 'the page kept the last run for the test').not.toBeNull();
  return last as LastRun;
}

/** Type a name into a box and take the first suggestion with the keyboard. */
async function choose(page: Page, box: number, text: string): Promise<void> {
  const input = page.locator('.place-box').nth(box).locator('input');
  await input.click();
  await input.pressSequentially(text, { delay: 20 });
  await expect(page.locator('.place-box').nth(box).locator('[role="option"]').first()).toBeVisible();
  await page.keyboard.press('ArrowDown');
  await expect(input).toHaveAttribute('aria-activedescendant', /option-0$/);
  await page.keyboard.press('Enter');
  await expect(page.locator('.place-box').nth(box)).toHaveAttribute('data-chosen', 'true');
}

async function waitForRun(page: Page): Promise<void> {
  await expect(page.locator('.explore')).toHaveAttribute('data-run-state', 'done', { timeout: RUN_TIMEOUT });
}

async function pause(page: Page): Promise<void> {
  const play = page.locator('.clock-play');
  if ((await play.getAttribute('aria-pressed')) === 'true') await play.click();
  await expect(play).toHaveAttribute('aria-pressed', 'false');
}

/** Each rule's readout at clock time t equals the engine's routes CSV at that second, and the
 * headline shows the fastest rule's numbers. */
async function expectShowsRun(page: Page, run: LastRun, t: number): Promise<string[]> {
  const rows = parseCsv(run.routesCsv);
  await seekReadouts(page, t);
  const utc = utcPlus(run.request.start_utc, Math.floor(t));
  const seen: string[] = [];
  for (const s of engine.strategies) {
    const row = rows.find((r) => r.from === run.names.from && r.to === run.names.to && r.strategy === s && r.utc === utc);
    expect(row, `the routes CSV has ${s} at ${utc}`).toBeDefined();
    const li = page.locator(`.readouts li[data-strategy="${s}"]`);
    if (row?.status === 'ok') {
      await expect(li).toHaveAttribute('data-state', 'route');
      await expect(li).toHaveAttribute('data-route', row.route ?? '');
      expect(Number(await li.getAttribute('data-delay-ms'))).toBe(Number(row.one_way_ms));
    } else {
      await expect(li).toHaveAttribute('data-state', 'drop');
      await expect(li).toHaveAttribute('data-cause', row?.drop_cause ?? '');
    }
    seen.push(`${s}:${row?.status ?? ''}`);
  }
  // The headline: the fastest rule's delay and satellites, the same as its readout.
  const fastest = page.locator('.readouts li[data-strategy="lowest-delay"]');
  const headline = page.locator('.headline');
  if ((await fastest.getAttribute('data-state')) === 'route') {
    await expect(headline).toHaveAttribute('data-state', 'route');
    await expect(headline).toHaveAttribute('data-delay-ms', (await fastest.getAttribute('data-delay-ms')) ?? '');
    const hops = ((await fastest.getAttribute('data-route')) ?? '').split(' ').length;
    await expect(headline).toHaveAttribute('data-satellites', String(hops));
    const readout = ((await fastest.locator('.readout-state').textContent()) ?? '').split(' one way')[0] ?? '';
    await expect(headline).toContainText(`about ${readout} one way, through ${hops} satellite`);
  } else {
    await expect(headline).toHaveAttribute('data-state', 'drop');
    await expect(headline).toHaveAttribute('data-cause', (await fastest.getAttribute('data-cause')) ?? '');
  }
  return seen;
}

test('the landing page is "Try it", playing the saved run, with the boxes, button and legend on the first screen', async ({ page }) => {
  const errors = watchErrors(page);
  await page.goto('./');
  await expect(page.locator('body')).toHaveAttribute('data-ready', 'true');
  await expect(page.locator('#story-menu a[aria-current="page"]')).toHaveAttribute('data-story', 'explore');
  const section = page.locator('.explore');
  await expect(section).toHaveAttribute('data-run-state', 'saved');
  const first = story.directions[0];
  await expect(section).toHaveAttribute('data-from', sites[first?.from ?? 0]?.name ?? '');
  await expect(page.locator('.place-box').nth(0).locator('input')).toHaveValue(new RegExp(`^${sites[first?.from ?? 0]?.name ?? ''}`));
  await expect(page.locator('.place-box').nth(1).locator('input')).toHaveValue(new RegExp(`^${sites[first?.to ?? 0]?.name ?? ''}`));
  // Playing: the clock moves without a press.
  await expect(page.locator('.clock-play')).toHaveAttribute('aria-pressed', 'true');
  const t0 = Number(await page.locator('.clock').getAttribute('data-t'));
  await expect.poll(async () => Number(await page.locator('.clock').getAttribute('data-t'))).toBeGreaterThan(t0);
  await expect(page.locator('.headline')).not.toBeEmpty();

  const height = page.viewportSize()?.height ?? 0;
  for (const selector of ['.place-box >> nth=0', '.place-box >> nth=1', '.trip-go', '.globe-legend li >> nth=0', '.clock-play']) {
    const box = await page.locator(selector).boundingBox();
    expect(box, `${selector} is on the page`).not.toBeNull();
    expect((box?.y ?? height) + (box?.height ?? 0), `${selector} is on the first screen`).toBeLessThanOrEqual(height);
  }
  const globe = await page.locator('.stage-globe').boundingBox();
  expect((globe?.y ?? height) + (globe?.height ?? 0) / 2).toBeLessThanOrEqual(height);

  expect(errors).toEqual([]);
});

test.describe('on a phone', () => {
  test.use({ viewport: { width: 390, height: 844 }, hasTouch: true });
  test('the boxes and the button are on the first screen, and nothing is wider than the window', async ({ page }) => {
    await page.goto('./');
    await expect(page.locator('body')).toHaveAttribute('data-ready', 'true');
    for (const selector of ['.place-box >> nth=0', '.place-box >> nth=1', '.trip-go']) {
      const box = await page.locator(selector).boundingBox();
      expect((box?.y ?? 900) + (box?.height ?? 0), `${selector} is on a phone's first screen`).toBeLessThanOrEqual(844);
    }
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(390);
    // The suggestions fit too.
    await page.locator('.place-box').nth(0).locator('input').fill('san');
    const list = await page.locator('.place-suggestions').first().boundingBox();
    expect((list?.x ?? 0) + (list?.width ?? 999)).toBeLessThanOrEqual(390);
  });
});

test('two cities typed by keyboard give a run worked out in the page, shown as the engine wrote it', async ({ page }) => {
  const errors = watchErrors(page);
  await open(page, 'explore');
  await choose(page, 0, 'lagos');
  await choose(page, 1, 'sydney');
  await expect(page.locator('.place-box').nth(1).locator('input')).toHaveValue('Sydney, Australia');
  // Enter in a box with a place chosen presses the button.
  await page.keyboard.press('Enter');
  await expect(page.locator('.explore')).toHaveAttribute('data-run-state', 'running');
  await waitForRun(page);

  const run = await lastRun(page);
  expect(run.exitCode).toBe(0);
  expect(run.request.start_utc).toBe(engine.window.start_utc);
  expect(run.request.duration_s).toBe(DURATION);
  expect(run.request.from.name).toBe('Lagos');
  expect(run.request.to.name).toBe('Sydney');
  await expect(page.locator('.explore')).toHaveAttribute('data-run', digest(run.routesCsv));
  await expect(page.locator('.explore')).toHaveAttribute('data-from', 'Lagos');
  await expect(page.locator('.run-note')).toContainText('Worked out just now in your browser');
  // Every rule has a row per second of the run, both ways.
  expect(parseCsv(run.routesCsv).length).toBe((DURATION + 1) * 2 * engine.strategies.length);

  await pause(page);
  const rows = parseCsv(run.routesCsv);
  const drop = rows.find((r) => r.from === run.names.from && r.to === run.names.to && r.status !== 'ok');
  const dropAt = drop === undefined ? null : (Date.parse(`${drop.utc ?? ''}Z`) - Date.parse(`${run.request.start_utc}Z`)) / 1000;
  // A second where the fastest rule's route or state differs from every other rule's, so a
  // headline that followed another rule would show other numbers.
  const at = (strategy: string, utc: string): string => {
    const r = rows.find((x) => x.from === run.names.from && x.to === run.names.to && x.strategy === strategy && x.utc === utc);
    return `${r?.status ?? ''} ${r?.route ?? ''} ${r?.one_way_ms ?? ''}`;
  };
  let differs: number | null = null;
  for (let k = 0; k <= DURATION && differs === null; k++) {
    const utc = utcPlus(run.request.start_utc, k);
    if (engine.strategies.every((s) => s === 'lowest-delay' || at(s, utc) !== at('lowest-delay', utc))) differs = k;
  }
  expect(differs, 'a second where the rules disagree').not.toBeNull();
  const times = [0, 37.5, DURATION / 2, DURATION, differs ?? 0, ...(dropAt === null ? [] : [dropAt, dropAt + 0.5])];
  for (const t of times) await expectShowsRun(page, run, t);

  // The other direction is the engine's other direction.
  await page.locator('.segmented[aria-label="Direction"] button').nth(1).click();
  await expect(page.locator('.explore')).toHaveAttribute('data-from', 'Sydney');
  await pause(page);
  await expectShowsRun(page, { ...run, names: { from: run.names.to, to: run.names.from } }, 60);
  expect(errors).toEqual([]);
});

test('a click on the globe sets the active box to the place under it, then To becomes active', async ({ page }) => {
  await open(page, 'explore');
  await pause(page);
  // The camera looks at the globe from above the middle of the saved run's two places, so the
  // middle of the globe's area is that point.
  const first = story.directions[0];
  const a = sites[first?.from ?? 0];
  const b = sites[first?.to ?? 0];
  const ecef = (p: { lat_deg: number; lon_deg: number } | undefined): number[] => {
    const lat = ((p?.lat_deg ?? 0) * Math.PI) / 180;
    const lon = ((p?.lon_deg ?? 0) * Math.PI) / 180;
    return [Math.cos(lat) * Math.cos(lon), Math.cos(lat) * Math.sin(lon), Math.sin(lat)];
  };
  const [x, y, z] = ecef(a).map((v, k) => v + (ecef(b)[k] ?? 0)) as [number, number, number];
  const lat = (Math.atan2(z, Math.hypot(x, y)) * 180) / Math.PI;
  const lon = (Math.atan2(y, x) * 180) / Math.PI;

  await expect(page.locator('.place-box').nth(0)).toHaveAttribute('data-active', 'true');
  await expect(page.locator('.pick-hint')).toHaveText('Click the globe to set From');
  const globe = await page.locator('.stage-globe').boundingBox();
  if (globe === null) throw new Error('no globe');
  await page.mouse.click(globe.x + globe.width / 2, globe.y + globe.height / 2);
  const input = page.locator('.place-box').nth(0).locator('input');
  await expect(input).not.toHaveValue(new RegExp(`^${a?.name ?? ''}`));
  const value = await input.inputValue();
  const m = /^Point at ([\d.]+)° ?([NS]?), ([\d.]+)° ?([EW]?)$/.exec(value);
  expect(m, `"${value}" names a point (the middle of the two places is far from any town)`).not.toBeNull();
  const shownLat = Number(m?.[1]) * (m?.[2] === 'S' ? -1 : 1);
  const shownLon = Number(m?.[3]) * (m?.[4] === 'W' ? -1 : 1);
  expect(Math.abs(shownLat - lat), `latitude ${shownLat} for ${lat}`).toBeLessThanOrEqual(0.2);
  expect(Math.abs(shownLon - lon), `longitude ${shownLon} for ${lon}`).toBeLessThanOrEqual(0.2);
  await expect(page.locator('.place-box').nth(1)).toHaveAttribute('data-active', 'true');
  await expect(page.locator('.pick-hint')).toHaveText('Click the globe to set To');

  // A drag turns the globe and sets nothing.
  const before = await page.locator('.place-box').nth(1).locator('input').inputValue();
  await page.mouse.move(globe.x + globe.width / 2, globe.y + globe.height / 2);
  await page.mouse.down();
  await page.mouse.move(globe.x + globe.width / 2 + 120, globe.y + globe.height / 2 + 40, { steps: 8 });
  await page.mouse.up();
  await expect(page.locator('.place-box').nth(1).locator('input')).toHaveValue(before);
});

test('the address keeps the choice, and opening it reproduces the run', async ({ page, context }) => {
  await open(page, 'explore');
  await choose(page, 0, 'reykjav');
  await choose(page, 1, 'tokyo');
  await page.locator('.trip-go').click();
  await waitForRun(page);
  const hash = await page.evaluate(() => location.hash);
  expect(hash).toMatch(/^#explore\?from=Reykjav%C3%ADk,64\.\d+,-21\.\d+&to=Tokyo,35\.\d+,139\.\d+$/);
  const run = await lastRun(page);

  const other = await context.newPage();
  await other.goto(`./${hash}`);
  await expect(other.locator('body')).toHaveAttribute('data-ready', 'true');
  await waitForRun(other);
  await expect(other.locator('.place-box').nth(0).locator('input')).toHaveValue('Reykjavík, Iceland');
  await expect(other.locator('.place-box').nth(1).locator('input')).toHaveValue('Tokyo, Japan');
  await expect(other.locator('.explore')).toHaveAttribute('data-run', digest(run.routesCsv));
  expect((await lastRun(other)).routesCsv).toBe(run.routesCsv);
});

test('the same place twice is refused in plain words, and nothing runs', async ({ page }) => {
  await open(page, 'explore');
  await choose(page, 0, 'lagos');
  await choose(page, 1, 'lagos');
  await page.locator('.trip-go').click();
  await expect(page.locator('.explore')).toHaveAttribute('data-run-state', 'refused');
  await expect(page.locator('.trip-status')).toContainText('From and To are the same place (Lagos). Choose two different places.');
  expect(await page.evaluate(() => (window as unknown as { __satsimExplore?: { last: unknown } }).__satsimExplore?.last ?? null)).toBeNull();
  await expect(page.locator('.explore')).not.toHaveAttribute('data-run', /./);
});

test('a place at the pole, where satellites are sometimes out of view, says so with the engine’s cause', async ({ page }) => {
  // No point of this snapshot is out of view for the whole run (the polar shell passes over
  // both poles), so this checks the gaps; a run with no connection at all is a unit test.
  await page.goto('./#explore?from=North%20Pole,90,0&to=London,51.5072,-0.1275');
  await expect(page.locator('body')).toHaveAttribute('data-ready', 'true');
  await waitForRun(page);
  const run = await lastRun(page);
  const drop = parseCsv(run.routesCsv).find((r) => r.from === run.names.from && r.strategy === 'lowest-delay' && r.status !== 'ok');
  expect(drop, 'the fastest rule has a moment without a connection').toBeDefined();
  await pause(page);
  const t = (Date.parse(`${drop?.utc ?? ''}Z`) - Date.parse(`${run.request.start_utc}Z`)) / 1000;
  await seekReadouts(page, t);
  const headline = page.locator('.headline');
  await expect(headline).toHaveAttribute('data-state', 'drop');
  await expect(headline).toHaveAttribute('data-cause', drop?.drop_cause ?? '');
  await expect(headline).toContainText('North Pole → London: no connection at this moment, because');
});

test('the headline’s exact delay follows the clock even when its rounded text stays the same', async ({ page }) => {
  // The fault: the headline was redrawn only when its text changed, and the text is rounded,
  // so its exact value (data-delay-ms) could stay that of an earlier second.
  await page.goto('./#explore?from=Lagos,6.4433,3.3915&to=Sydney,-33.92,151.1852');
  await expect(page.locator('body')).toHaveAttribute('data-ready', 'true');
  await waitForRun(page);
  const run = await lastRun(page);
  await pause(page);
  const rows = parseCsv(run.routesCsv).filter(
    (r) => r.from === run.names.from && r.to === run.names.to && r.strategy === 'lowest-delay',
  );
  // Two seconds in a row on one route whose delays differ but read the same on screen.
  const shown = (ms: string | undefined): string => Number(ms).toPrecision(3);
  const k = rows.findIndex((r, i) => {
    const next = rows[i + 1];
    return (
      next !== undefined &&
      r.status === 'ok' &&
      next.status === 'ok' &&
      r.route === next.route &&
      r.one_way_ms !== next.one_way_ms &&
      shown(r.one_way_ms) === shown(next.one_way_ms)
    );
  });
  expect(k, 'two seconds with one rounded delay').toBeGreaterThanOrEqual(0);
  const headline = page.locator('.headline');
  for (const step of [k, k + 1]) {
    await seekReadouts(page, step);
    await expect(headline).toHaveAttribute('data-delay-ms', String(Number(rows[step]?.one_way_ms)), { timeout: 10_000 });
  }
});

test('the page keeps animating while the engine works', async ({ page }) => {
  await open(page, 'explore');
  await page.locator('.trip-example').first().click();
  const section = page.locator('.explore');
  await expect(section).toHaveAttribute('data-run-state', 'running');
  await expect(page.locator('.trip-go')).toHaveText('Cancel');
  await expect(page.locator('.trip-status')).toContainText(/Loading the simulator|Working out the route/);
  // Frames and the saved run's clock both advance during the run.
  await expect(page.locator('.trip-status')).toContainText('Working out the route', { timeout: RUN_TIMEOUT });
  const sample = await page.evaluate(
    () =>
      new Promise<{ frames: number; t0: number; t1: number }>((resolve) => {
        const clock = document.querySelector('.clock') as HTMLElement;
        const t0 = Number(clock.dataset.t);
        let frames = 0;
        const start = performance.now();
        const step = (now: number): void => {
          frames++;
          if (now - start < 1000) requestAnimationFrame(step);
          else resolve({ frames, t0, t1: Number(clock.dataset.t) });
        };
        requestAnimationFrame(step);
      }),
  );
  expect(await section.getAttribute('data-run-state'), 'the engine was still working').toBe('running');
  // Not frozen: with the engine on the page's own thread no frame is drawn until the run ends.
  // The limit is low because software WebGL on a small machine draws about 3 frames a second
  // (measured on GitHub's runner; about 30 on a 4-core cloud machine).
  expect(sample.frames, 'frames in one second during the run').toBeGreaterThanOrEqual(2);
  expect(sample.t1).toBeGreaterThan(sample.t0);
  await waitForRun(page);
  await expect(page.locator('.trip-go')).toHaveText('Show the route');
});

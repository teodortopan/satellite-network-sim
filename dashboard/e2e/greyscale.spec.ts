// The page is black, white and greys only: no pixel of any story has a hue. Things are told
// apart by line style, width, dot shape and brightness, each named in text beside it. A
// full-page shot covers the text, charts and controls; a globe out of the window may not be
// drawn in it, so each globe is also checked scrolled into the window.

import { test } from '@playwright/test';
import { expectNoHue, open } from './helpers';

const GLOBES: Record<string, string> = {
  explore: '.stage-globe',
  failure: '.lanes',
  route: '.stage-globe',
  constellation: '.stage-globe',
  resilience: '.instant .stage-globe',
};

for (const [story, globe] of Object.entries(GLOBES)) {
  test(`the ${story} story has no colour with a hue`, async ({ page }) => {
    await open(page, story);
    await expectNoHue(page);
    await page.locator(globe).first().scrollIntoViewIfNeeded();
    await page.waitForTimeout(300);
    await expectNoHue(page, 3, false);
  });
}

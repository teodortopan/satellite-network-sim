import { existsSync } from 'node:fs';
import { defineConfig, devices } from '@playwright/test';

// The browser test runs against the built page (npm run build) with the exported data in it.
// Without the data it stops here, before any browser starts, saying what to run.
if (!existsSync('dist/data/manifest.json')) {
  throw new Error(
    'dist/data/manifest.json is missing. Export the data (python3 python/export_dashboard.py ' +
      '--small, in the CI image), then build the page (npm run build), then run npm run e2e.',
  );
}

const PORT = 4173;

export default defineConfig({
  testDir: 'e2e',
  timeout: 90_000,
  expect: { timeout: 30_000 },
  fullyParallel: false,
  workers: 1,
  forbidOnly: true,
  retries: 0,
  reporter: [['list']],
  use: {
    baseURL: `http://127.0.0.1:${PORT}/`,
    trace: 'off',
    ...devices['Desktop Chrome'],
    viewport: { width: 1440, height: 900 },
    launchOptions: {
      // Headless Chromium has no GPU here; SwiftShader draws WebGL in software.
      args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
    },
  },
  webServer: {
    command: `npx vite preview --port ${PORT} --strictPort --host 127.0.0.1`,
    url: `http://127.0.0.1:${PORT}/`,
    timeout: 30_000,
    reuseExistingServer: false,
  },
});

import { defineConfig } from 'vitest/config';

export default defineConfig({
  // Relative paths only, so the built site works under /satellite-network-sim/ on GitHub
  // Pages and at the root of any other host (spec 4.9).
  base: './',
  // The engine's Web Worker is an ES module (src/engine/worker.ts, created with type 'module').
  worker: { format: 'es' },
  build: {
    // The exported data in public/data/ is copied as it is; nothing else is inlined from it.
    chunkSizeWarningLimit: 1000,
  },
  test: {
    include: ['src/**/*.test.ts'],
    environment: 'node',
  },
});

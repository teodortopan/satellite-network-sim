// The engine's Web Worker: runs the routing program in WebAssembly (run.ts) away from the
// page's thread, so the globe keeps turning while it works. It gets the program and its inputs
// once (an 'init' message), keeps them, and answers each 'run' with progress and the result.
// The program's ES module arrives as text and is imported from a blob: URL; it never needs to
// find its .wasm file, because the compiled module is handed to it (instantiateWasm).

// Typed with the page's DOM library (no webworker library, which clashes with it): the
// worker uses only self.postMessage, addEventListener, Blob, URL and import().

import type { FromWorker, InitMessage, RunMessage, ToWorker } from './protocol';
import { EngineRequestError, runEngine, type EngineFactory } from './run';

interface Loaded {
  init: InitMessage;
  factory: Promise<EngineFactory>;
}

let loaded: Loaded | null = null;

function post(message: FromWorker): void {
  self.postMessage(message);
}

async function importProgram(text: string): Promise<EngineFactory> {
  const url = URL.createObjectURL(new Blob([text], { type: 'text/javascript' }));
  try {
    const module = (await import(/* @vite-ignore */ url)) as { default: EngineFactory };
    return module.default;
  } finally {
    URL.revokeObjectURL(url);
  }
}

async function run(message: RunMessage): Promise<void> {
  if (loaded === null) {
    post({ type: 'error', id: message.id, kind: 'failed', message: 'the simulator was not loaded' });
    return;
  }
  try {
    const result = await runEngine({
      factory: await loaded.factory,
      wasm: loaded.init.wasm,
      files: loaded.init.files,
      engine: loaded.init.engine,
      from: message.from,
      to: message.to,
      start_utc: message.start_utc,
      duration_s: message.duration_s,
      strategies: message.strategies,
      onProgress: (done, total) => post({ type: 'progress', id: message.id, done, total }),
    });
    post({ type: 'result', id: message.id, result });
  } catch (error) {
    post({
      type: 'error',
      id: message.id,
      kind: error instanceof EngineRequestError ? 'request' : 'failed',
      message: error instanceof Error ? error.message : String(error),
    });
  }
}

self.addEventListener('message', (event: MessageEvent<ToWorker>) => {
  const message = event.data;
  if (message.type === 'init') {
    loaded = { init: message, factory: importProgram(message.programJs) };
    // A failed import is reported with the first run.
    loaded.factory.catch(() => undefined);
  } else {
    void run(message);
  }
});

// The messages between the page (engine/client.ts) and the engine's Web Worker
// (engine/worker.ts). The page downloads and compiles once and hands the worker everything it
// needs; the worker only runs.

import type { EnginePoint, EngineResult } from './run';
import type { EngineManifest } from './settings';

/** Sent once to each new worker: the program and its inputs, already downloaded and checked. */
export interface InitMessage {
  type: 'init';
  /** The text of satsim-routing.js (an ES module), imported in the worker from a blob: URL. */
  programJs: string;
  /** satsim-routing.wasm, compiled once on the page (a module is shared, not copied). */
  wasm: WebAssembly.Module;
  /** Every input file by its path in the export. */
  files: Map<string, Uint8Array>;
  engine: EngineManifest;
}

export interface RunMessage {
  type: 'run';
  id: number;
  from: EnginePoint;
  to: EnginePoint;
  start_utc: string;
  duration_s: number;
  strategies: string[];
}

export type ToWorker = InitMessage | RunMessage;

export type FromWorker =
  | { type: 'progress'; id: number; done: number; total: number }
  | { type: 'result'; id: number; result: EngineResult }
  /** `kind`: 'request' for a request refused before the program started, else 'failed'. */
  | { type: 'error'; id: number; kind: 'request' | 'failed'; message: string };

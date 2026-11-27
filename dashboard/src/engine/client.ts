// The page's side of the engine: downloads the routing program and its inputs once (through
// the manifest, so a file of the wrong size is refused as every other file is), compiles the
// WebAssembly once, and runs one request at a time in a Web Worker (worker.ts).
//
// A new request while one runs, or cancel(), stops the worker at once (terminate) and starts a
// fresh one for the next request. The downloaded bytes and the compiled module stay here on the
// page, so a fresh worker gets them in a message: nothing is downloaded or compiled twice.

import { DataError } from '../data/check';
import type { DataSource } from '../data/loader';
import type { FromWorker, InitMessage, RunMessage } from './protocol';
import type { EnginePoint, EngineResult } from './run';

export interface EngineRun {
  from: EnginePoint;
  to: EnginePoint;
  start_utc: string;
  duration_s: number;
  strategies: readonly string[];
}

/** 'loading': the files are downloading (the first run only); 'running': the program steps. */
export type EnginePhase = 'loading' | 'running';

export type EngineFailureKind = 'download' | 'request' | 'failed';

/** Why a run gave no result. `cancelled` is not a failure: a newer request replaced it. */
export class EngineFailure extends Error {
  constructor(
    readonly kind: EngineFailureKind | 'cancelled',
    message: string,
  ) {
    super(message);
    this.name = 'EngineFailure';
  }
}

/** Whether this browser can run the engine: WebAssembly and module Web Workers. */
export function engineSupported(): boolean {
  if (typeof WebAssembly !== 'object' || typeof WebAssembly.compile !== 'function') return false;
  if (typeof Worker !== 'function') return false;
  // A browser that knows module workers reads the `type` option; one that does not ignores it.
  let readsType = false;
  const options = {
    get type(): 'module' {
      readsType = true;
      return 'module';
    },
  };
  try {
    new Worker('data:text/javascript,', options).terminate();
  } catch {
    return false;
  }
  return readsType;
}

interface Pending {
  id: number;
  resolve: (result: EngineResult) => void;
  reject: (error: EngineFailure) => void;
  onProgress: (phase: EnginePhase, fraction: number) => void;
}

export class EngineClient {
  private loading: Promise<Omit<InitMessage, 'type'>> | null = null;
  private worker: Worker | null = null;
  private pending: Pending | null = null;
  private nextId = 1;

  constructor(
    private readonly source: DataSource,
    private readonly makeWorker: () => Worker = () => new Worker(new URL('./worker.ts', import.meta.url), { type: 'module' }),
  ) {}

  /** Whether the program and its inputs are downloaded already. */
  get loaded(): boolean {
    return this.loading !== null;
  }

  /** Runs one request; a run still going is cancelled first. */
  async run(request: EngineRun, onProgress: (phase: EnginePhase, fraction: number) => void): Promise<EngineResult> {
    this.cancel();
    const id = this.nextId++;
    const result = new Promise<EngineResult>((resolve, reject) => {
      this.pending = { id, resolve, reject, onProgress };
    });
    try {
      const init = await this.load((fraction) => {
        if (this.pending?.id === id) onProgress('loading', fraction);
      });
      if (this.pending?.id !== id) return await result;
      onProgress('running', 0);
      const worker = this.workerFor(init);
      const message: RunMessage = { type: 'run', id, ...request, strategies: [...request.strategies] };
      worker.postMessage(message);
    } catch (error) {
      this.settle(id, (p) => {
        p.reject(error instanceof EngineFailure ? error : new EngineFailure('download', describe(error)));
      });
    }
    return result;
  }

  /** Stops the run going, if any; its promise is rejected with kind 'cancelled'. */
  cancel(): void {
    const p = this.pending;
    if (p === null) return;
    this.pending = null;
    this.worker?.terminate();
    this.worker = null;
    p.reject(new EngineFailure('cancelled', 'replaced by a newer request'));
  }

  dispose(): void {
    this.cancel();
    this.worker?.terminate();
    this.worker = null;
  }

  private settle(id: number, act: (p: Pending) => void): void {
    const p = this.pending;
    if (p === null || p.id !== id) return;
    this.pending = null;
    act(p);
  }

  /** The program and its inputs, downloaded and compiled once; a failure lets the next run retry. */
  private load(onProgress: (fraction: number) => void): Promise<Omit<InitMessage, 'type'>> {
    if (this.loading === null) {
      this.loading = this.download(onProgress);
      this.loading.catch(() => {
        this.loading = null;
      });
    } else {
      onProgress(1);
    }
    return this.loading;
  }

  private async download(onProgress: (fraction: number) => void): Promise<Omit<InitMessage, 'type'>> {
    const { engine, files: listed } = this.source.manifest;
    const paths = [engine.program.js, engine.program.wasm, ...engine.snapshot, ...engine.ground];
    const size = (path: string): number => listed.find((f) => f.path === path)?.bytes ?? 0;
    const total = paths.reduce((sum, p) => sum + size(p), 0);
    let done = 0;
    onProgress(0);
    const buffers = await Promise.all(
      paths.map(async (path) => {
        const buffer = await this.source.bytes(path);
        done += size(path);
        onProgress(total > 0 ? done / total : 1);
        return buffer;
      }),
    );
    const [js, wasm, ...inputs] = buffers as [ArrayBuffer, ArrayBuffer, ...ArrayBuffer[]];
    let module: WebAssembly.Module;
    try {
      module = await WebAssembly.compile(wasm);
    } catch (error) {
      throw new EngineFailure('download', `${engine.program.wasm} could not be compiled (${describe(error)})`);
    }
    const files = new Map<string, Uint8Array>();
    paths.slice(2).forEach((path, k) => files.set(path, new Uint8Array(inputs[k] as ArrayBuffer)));
    return { programJs: new TextDecoder().decode(js), wasm: module, files, engine };
  }

  private workerFor(init: Omit<InitMessage, 'type'>): Worker {
    if (this.worker !== null) return this.worker;
    const worker = this.makeWorker();
    worker.addEventListener('message', (event: MessageEvent<FromWorker>) => {
      const m = event.data;
      if (m.type === 'progress') {
        if (this.pending?.id === m.id) this.pending.onProgress('running', m.total > 0 ? m.done / m.total : 0);
      } else if (m.type === 'result') {
        this.settle(m.id, (p) => p.resolve(m.result));
      } else {
        this.settle(m.id, (p) => p.reject(new EngineFailure(m.kind, m.message)));
      }
    });
    worker.addEventListener('error', (event) => {
      const id = this.pending?.id;
      this.worker?.terminate();
      this.worker = null;
      if (id !== undefined) this.settle(id, (p) => p.reject(new EngineFailure('failed', event.message || 'the simulator stopped')));
    });
    const message: InitMessage = { type: 'init', ...init };
    worker.postMessage(message);
    this.worker = worker;
    return worker;
  }
}

function describe(error: unknown): string {
  if (error instanceof DataError) return `${error.file} ${error.problem}`;
  return error instanceof Error ? error.message : String(error);
}

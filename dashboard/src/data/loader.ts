// Loading the exported data (spec 4.4 "Loading"): the manifest first, then every other file
// through it. A file whose size is not the manifest's is refused before it is read.

import { Check, DataError } from './check';
import { parseManifest, type Manifest } from './manifest';

export type Fetcher = (url: string) => Promise<Response>;

export const MANIFEST = 'manifest.json';

export class DataSource {
  constructor(
    readonly baseUrl: string,
    readonly manifest: Manifest,
    private readonly fetcher: Fetcher,
  ) {}

  /** The bytes of `path`, refused unless the manifest lists it with exactly this size. */
  async bytes(path: string): Promise<ArrayBuffer> {
    const entry = this.manifest.files.find((f) => f.path === path);
    if (entry === undefined) throw new DataError(path, 'is not listed in manifest.json');
    const buffer = await download(this.fetcher, this.baseUrl + path, path);
    if (buffer.byteLength !== entry.bytes) {
      throw new DataError(
        path,
        `has ${buffer.byteLength} bytes, but manifest.json says ${entry.bytes}; ` +
          'the data is incomplete or from another export',
      );
    }
    return buffer;
  }

  /** A JSON file, size-checked, then shape-checked by `parse`. */
  async json<T>(path: string, parse: (value: unknown, check: Check) => T): Promise<T> {
    return parse(decodeJson(await this.bytes(path), path), new Check(path));
  }
}

export async function openData(baseUrl: string, fetcher: Fetcher): Promise<DataSource> {
  const buffer = await download(fetcher, baseUrl + MANIFEST, MANIFEST);
  const manifest = parseManifest(decodeJson(buffer, MANIFEST), new Check(MANIFEST));
  return new DataSource(baseUrl, manifest, fetcher);
}

async function download(fetcher: Fetcher, url: string, file: string): Promise<ArrayBuffer> {
  let response: Response;
  try {
    response = await fetcher(url);
  } catch (error) {
    throw new DataError(file, `could not be downloaded (${String(error)})`);
  }
  if (!response.ok) {
    throw new DataError(file, `could not be downloaded (the server answered ${response.status})`);
  }
  return response.arrayBuffer();
}

function decodeJson(buffer: ArrayBuffer, file: string): unknown {
  try {
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(buffer));
  } catch {
    throw new DataError(file, 'is not valid JSON');
  }
}

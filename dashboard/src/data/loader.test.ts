import { describe, expect, it } from 'vitest';
import { DataError } from './check';
import { loadCommon, loadPositions, parseLinks, parseSatellites, parseSites } from './common';
import { openData, type Fetcher } from './loader';

const encoder = new TextEncoder();

/** A tiny export: 2 satellites, 2 samples 60 s apart, in one position file. */
function exportFiles(): Map<string, Uint8Array<ArrayBuffer>> {
  const files = new Map<string, Uint8Array<ArrayBuffer>>();
  const positions = new DataView(new ArrayBuffer(2 * 2 * 3 * 4));
  [7000, 0, 0, 0, 7000, 0, 6999, 10, 0, -10, 6999, 0].forEach((v, i) => {
    positions.setFloat32(4 * i, v, true);
  });
  files.set('positions-0.bin', new Uint8Array(positions.buffer));
  files.set(
    'satellites.json',
    encoder.encode(
      JSON.stringify({
        shells: ['53deg'],
        norad_cat_id: [1, 2],
        name: ['A', 'B'],
        shell: [0, 0],
        plane: [0, 1],
        index_in_plane: [0, 0],
      }),
    ),
  );
  files.set(
    'sites.json',
    encoder.encode(JSON.stringify({ sites: [{ name: 'Paris', kind: 'city', lat_deg: 48.86, lon_deg: 2.35 }] })),
  );
  files.set('links-three.json', encoder.encode(JSON.stringify({ pattern: 'three', links: [[0, 1]] })));
  return files;
}

function manifestFor(files: Map<string, Uint8Array<ArrayBuffer>>, sizeOverride: Record<string, number> = {}): unknown {
  return {
    format_version: 1,
    snapshot: '2026-11-18',
    ground: '2026-11-19',
    settings: {
      mode: 'small',
      plan_time_utc: '2026-11-18T21:15:00',
      window_start_utc: '2026-11-18T21:30:30',
      window_s: 60,
      sample_step_s: 60,
      samples: 2,
      error_window_s: 60,
      error_step_s: 1,
    },
    satellites: 2,
    positions: [{ path: 'positions-0.bin', first_sample: 0, samples: 2 }],
    files: [...files].map(([path, bytes]) => ({ path, bytes: sizeOverride[path] ?? bytes.length, sha256: '0' })),
  };
}

function fetcherFor(files: Map<string, Uint8Array<ArrayBuffer>>, manifest: unknown): Fetcher {
  return (url) => {
    const name = url.replace(/^data\//, '');
    const body = name === 'manifest.json' ? encoder.encode(JSON.stringify(manifest)) : files.get(name);
    return Promise.resolve(body === undefined ? new Response(null, { status: 404 }) : new Response(body));
  };
}

async function refusal(promise: Promise<unknown>): Promise<DataError> {
  try {
    await promise;
  } catch (error) {
    if (error instanceof DataError) return error;
    throw error;
  }
  throw new Error('the file was accepted');
}

describe('loaders', () => {
  it('load a good export', async () => {
    const files = exportFiles();
    const source = await openData('data/', fetcherFor(files, manifestFor(files)));
    const common = await loadCommon(source);
    expect(common.satellites.name).toEqual(['A', 'B']);
    expect(common.sites[0]?.name).toBe('Paris');
    expect(Array.from(common.positions.subarray(0, 6))).toEqual([7000, 0, 0, 0, 7000, 0]);
    const links = await source.json('links-three.json', (v, c) => parseLinks(v, c, 2));
    expect(Array.from(links.pairs)).toEqual([0, 1]);
  });

  it('refuse a file whose size is not the manifest size', async () => {
    const files = exportFiles();
    const sites = files.get('sites.json')?.length ?? 0;
    const source = await openData('data/', fetcherFor(files, manifestFor(files, { 'sites.json': sites + 1 })));
    const error = await refusal(source.json('sites.json', parseSites));
    expect(error.file).toBe('sites.json');
    expect(error.problem).toMatch(/bytes, but manifest.json says/);
  });

  it('refuse a position file whose size is not the manifest size', async () => {
    const files = exportFiles();
    const manifest = manifestFor(files) as { files: { path: string; bytes: number }[] };
    files.set('positions-0.bin', new Uint8Array(40));
    const source = await openData('data/', fetcherFor(files, manifest));
    const error = await refusal(loadPositions(source));
    expect(error.file).toBe('positions-0.bin');
  });

  it('refuse a file the manifest does not list', async () => {
    const files = exportFiles();
    const source = await openData('data/', fetcherFor(files, manifestFor(files)));
    const error = await refusal(source.bytes('other.json'));
    expect(error.problem).toMatch(/not listed/);
  });

  it('refuse a missing field', async () => {
    const files = exportFiles();
    files.set('satellites.json', encoder.encode(JSON.stringify({ shells: ['53deg'], norad_cat_id: [1, 2], name: ['A', 'B'], shell: [0, 0] })));
    const source = await openData('data/', fetcherFor(files, manifestFor(files)));
    const error = await refusal(source.json('satellites.json', (v, c) => parseSatellites(v, c, 2)));
    expect(error.problem).toBe('plane is missing');
  });

  it('refuse an index out of range', async () => {
    const files = exportFiles();
    files.set('links-three.json', encoder.encode(JSON.stringify({ pattern: 'three', links: [[0, 2]] })));
    const source = await openData('data/', fetcherFor(files, manifestFor(files)));
    const error = await refusal(source.json('links-three.json', (v, c) => parseLinks(v, c, 2)));
    expect(error.problem).toBe('links[0][1] is 2, but there are only 2 satellites (indices 0 to 1)');
  });

  it('refuse a manifest whose position files do not cover every sample', async () => {
    const files = exportFiles();
    const manifest = manifestFor(files) as { positions: { samples: number }[] };
    (manifest.positions[0] as { samples: number }).samples = 1;
    const error = await refusal(openData('data/', fetcherFor(files, manifest)));
    expect(error.file).toBe('manifest.json');
  });

  it('report a file that cannot be downloaded', async () => {
    const files = exportFiles();
    const manifest = manifestFor(files);
    files.delete('sites.json');
    const source = await openData('data/', fetcherFor(files, manifest));
    const error = await refusal(source.json('sites.json', parseSites));
    expect(error.problem).toMatch(/could not be downloaded \(the server answered 404\)/);
  });

  it('refuse text that is not JSON', async () => {
    const files = exportFiles();
    files.set('sites.json', encoder.encode('{"sites": ['));
    const source = await openData('data/', fetcherFor(files, manifestFor(files)));
    const error = await refusal(source.json('sites.json', parseSites));
    expect(error.problem).toBe('is not valid JSON');
  });
});

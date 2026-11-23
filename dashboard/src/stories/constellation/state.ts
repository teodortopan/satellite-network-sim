// What the constellation story shows for a menu choice: which satellites are chosen (all, one
// shell, one plane of a shell), and which planned links join two chosen satellites. Lookups
// and filters only.

import type { Satellites } from '../../data/common';

/** 1 for each chosen satellite: every one, one shell, or one plane of one shell. */
export function chosen(satellites: Satellites, shell: number | null, plane: number | null): Uint8Array {
  const out = new Uint8Array(satellites.shell.length);
  satellites.shell.forEach((s, i) => {
    if ((shell === null || s === shell) && (plane === null || satellites.plane[i] === plane)) out[i] = 1;
  });
  return out;
}

/**
 * The links (index pairs, in file order) to draw for a choice: those with both satellites
 * chosen, or with `either` those with at least one (so one plane shows the links that leave it).
 */
export function linksOf(pairs: Uint32Array, picked: Uint8Array, either: boolean): Uint32Array {
  const out: number[] = [];
  for (let k = 0; k < pairs.length; k += 2) {
    const a = pairs[k] as number;
    const b = pairs[k + 1] as number;
    const n = (picked[a] as number) + (picked[b] as number);
    if (n === 2 || (either && n === 1)) out.push(a, b);
  }
  return Uint32Array.from(out);
}

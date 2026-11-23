// Every satellite's position at the clock's time, in scene coordinates, shared by all the views
// of a story; and the index lists that leave out satellites (and their links) with no position.

import { BufferAttribute, Color } from 'three';
import type { CommonData } from '../data/common';
import { shellColor } from '../theme';
import { ecefArrayToScene } from './coords';
import { positionsAt, samplePair } from './interpolation';
import { sharedPositions } from './layers';

export class SatelliteField {
  readonly count: number;
  readonly position: BufferAttribute;
  /** The drawable satellites, as an index list for a points object. */
  readonly pointIndex: BufferAttribute;
  /** The planned links whose two satellites are drawable, as an index list for segments. */
  readonly linkIndex: BufferAttribute;
  /** RGB per satellite: its shell's colour. */
  readonly shellColors: Float32Array;
  /** How many entries of pointIndex and linkIndex are in use (for draw ranges). */
  pointCount = 0;
  linkIndexCount = 0;
  private readonly ecef: Float32Array;
  private readonly drawable: Uint8Array;
  private readonly lastDrawable: Uint8Array;
  private lastSample = -1;

  constructor(
    private readonly data: CommonData,
    private readonly linkPairs: Uint32Array,
  ) {
    this.count = data.satellites.norad.length;
    this.position = sharedPositions(this.count);
    this.pointIndex = new BufferAttribute(new Uint32Array(this.count), 1);
    this.linkIndex = new BufferAttribute(new Uint32Array(linkPairs.length), 1);
    this.ecef = new Float32Array(this.count * 3);
    this.drawable = new Uint8Array(this.count);
    this.lastDrawable = new Uint8Array(this.count).fill(2);
    this.shellColors = new Float32Array(this.count * 3);
    const color = new Color();
    data.satellites.shell.forEach((shell, i) => {
      color.set(shellColor(shell));
      this.shellColors.set([color.r, color.g, color.b], 3 * i);
    });
  }

  /** Move every satellite to window time `t_s`. */
  update(t_s: number): void {
    const { settings } = this.data.source.manifest;
    positionsAt(
      this.data.positions,
      this.count,
      settings.samples,
      settings.sample_step_s,
      t_s,
      this.ecef,
      this.drawable,
    );
    ecefArrayToScene(this.ecef, this.position.array as Float32Array, this.count);
    this.position.needsUpdate = true;
    const { s0 } = samplePair(t_s, settings.sample_step_s, settings.samples);
    if (s0 !== this.lastSample) {
      this.lastSample = s0;
      this.updateIndices();
    }
  }

  isDrawable(sat: number): boolean {
    return this.drawable[sat] === 1;
  }

  private updateIndices(): void {
    if (this.drawable.every((d, i) => d === this.lastDrawable[i])) return;
    this.lastDrawable.set(this.drawable);
    const points = this.pointIndex.array as Uint32Array;
    let n = 0;
    for (let i = 0; i < this.count; i++) if (this.drawable[i] === 1) points[n++] = i;
    this.pointIndex.needsUpdate = true;
    this.pointCount = n;
    const links = this.linkIndex.array as Uint32Array;
    let m = 0;
    for (let k = 0; k < this.linkPairs.length; k += 2) {
      const a = this.linkPairs[k] as number;
      const b = this.linkPairs[k + 1] as number;
      if (this.drawable[a] === 1 && this.drawable[b] === 1) {
        links[m++] = a;
        links[m++] = b;
      }
    }
    this.linkIndex.needsUpdate = true;
    this.linkIndexCount = m;
  }
}

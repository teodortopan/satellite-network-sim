// Ground sites and routes in scene coordinates, shared by every story that draws a route: each
// site's position just above the ground, and the points of a path through sites and
// satellites at the clock's time.

import type { CommonData } from '../data/common';
import { latLonToScene } from './coords';
import { surfaceArc } from './layers';
import type { SatelliteField } from './satellite-field';

export const SITE_RADIUS = 1.002;

/** A point given by latitude and longitude (degrees), in scene coordinates just above the ground. */
export function pointScenePosition(p: { lat_deg: number; lon_deg: number }): [number, number, number] {
  return latLonToScene(p.lat_deg, p.lon_deg, SITE_RADIUS);
}

/** Scene positions of every site, just above the ground. */
export function siteScenePositions(data: CommonData): Float32Array {
  const out = new Float32Array(data.sites.length * 3);
  data.sites.forEach((s, i) => out.set(latLonToScene(s.lat_deg, s.lon_deg, SITE_RADIUS), 3 * i));
  return out;
}

export type PathNode = { kind: 'site'; site: number } | { kind: 'sat'; sat: number };

/** A route of satellites between two sites, as path nodes. */
export function routeNodes(from: number, route: readonly number[], to: number | null): PathNode[] {
  const nodes: PathNode[] = [{ kind: 'site', site: from }];
  for (const sat of route) nodes.push({ kind: 'sat', sat });
  if (to !== null) nodes.push({ kind: 'site', site: to });
  return nodes;
}

/**
 * Where the nodes of a path are at the clock's time. Sites are numbered as in `sites` (the
 * saved ground sites); extra places (points chosen by coordinate, such as the two ends of a
 * route between any two points) are numbered after them: `siteCount`, `siteCount + 1`, ...
 */
export class PathPlacer {
  private extra = new Float32Array(0);

  constructor(
    private readonly field: SatelliteField,
    private readonly sites: Float32Array,
  ) {}

  /** The number of saved sites; the first extra place has this number. */
  get siteCount(): number {
    return this.sites.length / 3;
  }

  /** Replace the extra places (x y z each, scene coordinates); returns the first one's number. */
  setExtraSites(positions: readonly number[] | Float32Array): number {
    if (positions.length % 3 !== 0) throw new RangeError('extra places come as x y z triples');
    this.extra = Float32Array.from(positions);
    return this.siteCount;
  }

  site(i: number): number[] {
    const n = this.siteCount;
    if (!Number.isInteger(i) || i < 0 || i >= n + this.extra.length / 3) throw new RangeError(`there is no place ${i}`);
    if (i < n) return Array.from(this.sites.subarray(3 * i, 3 * i + 3));
    const k = i - n;
    return Array.from(this.extra.subarray(3 * k, 3 * k + 3));
  }

  /** Where a node is now; null for a satellite with no position at this step. */
  node(node: PathNode): number[] | null {
    if (node.kind === 'site') return this.site(node.site);
    if (!this.field.isDrawable(node.sat)) return null;
    return Array.from((this.field.position.array as Float32Array).subarray(3 * node.sat, 3 * node.sat + 3));
  }

  /** The points of a path at the clock's time, skipping satellites that are not drawn. */
  points(nodes: readonly PathNode[]): number[] {
    const out: number[] = [];
    for (const n of nodes) {
      const p = this.node(n);
      if (p !== null) out.push(...p);
    }
    return out;
  }

  /** Points along the ground from site to site (fibre), one great-circle arc per leg. */
  ground(sites: readonly number[]): number[] {
    const out: number[] = [];
    for (let k = 1; k < sites.length; k++) {
      const arc = surfaceArc(this.site(sites[k - 1] as number), this.site(sites[k] as number), SITE_RADIUS);
      out.push(...(k === 1 ? arc : arc.slice(3)));
    }
    return out;
  }
}

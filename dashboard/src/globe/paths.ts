// Ground sites and routes in scene coordinates, shared by every story that draws a route: each
// site's position just above the ground, and the points of a path through sites and
// satellites at the clock's time.

import type { CommonData } from '../data/common';
import { latLonToScene } from './coords';
import { surfaceArc } from './layers';
import type { SatelliteField } from './satellite-field';

export const SITE_RADIUS = 1.002;

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

export class PathPlacer {
  constructor(
    private readonly field: SatelliteField,
    private readonly sites: Float32Array,
  ) {}

  site(i: number): number[] {
    return Array.from(this.sites.subarray(3 * i, 3 * i + 3));
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

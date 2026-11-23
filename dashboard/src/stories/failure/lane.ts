// One lane of the failure story: a globe view of one reaction mode, with the direction's route
// in the state the engine gives it at the clock's time, the baseline route faint beneath,
// failed elements, and the news as each satellite or site learns it.

import {
  BufferAttribute,
  Color,
  LineSegments,
  Mesh,
  Points,
  Scene,
  type BufferGeometry,
  type Group,
  type Material,
  type ShaderMaterial,
} from 'three';
import type { CommonData } from '../../data/common';
import { latLonToScene } from '../../globe/coords';
import type { View } from '../../globe/globe-view';
import { dotGeometry, dotMaterial, dots, linkSegments, NEVER, surfaceArc, ThickPath } from '../../globe/layers';
import type { SatelliteField } from '../../globe/satellite-field';
import { formatCount, formatInstant, formatMs } from '../../format';
import { THEME } from '../../theme';
import type { Interval, LaneState, Mode, Scenario } from './data';
import {
  drawnPath,
  firstDetection,
  firstKnownFact,
  intervalAt,
  newPlanAt,
  satelliteDetections,
  type DrawnPath,
  type PathNode,
} from './state';

const SITE_RADIUS = 1.002;
const SAT_SIZE = 2.2;

export const MODE_TITLES: Record<Mode, string> = {
  planner: 'Planner',
  'local-repair': 'Local repair',
  flooding: 'Flooding',
};

const MODE_NOTES: Record<Mode, string> = {
  planner: 'Only the planner on the ground changes routes, once reports reach it.',
  'local-repair': 'The node before the failure switches to a prepared backup; the planner follows.',
  flooding: 'News spreads from node to node; every node works out its own routes.',
};

export const STATE_TITLES: Record<LaneState, string> = {
  delivered: 'Delivered',
  black_hole: 'Black hole',
  loop: 'Loop',
  no_route: 'No route',
};

const STATE_COLORS: Record<LaneState, string> = {
  delivered: THEME.delivered,
  black_hole: THEME.blackHole,
  loop: THEME.loop,
  no_route: THEME.dim,
};

/** Scene positions of every site, just above the ground. */
export function siteScenePositions(data: CommonData): Float32Array {
  const out = new Float32Array(data.sites.length * 3);
  data.sites.forEach((s, i) => out.set(latLonToScene(s.lat_deg, s.lon_deg, SITE_RADIUS), 3 * i));
  return out;
}

interface RouteLayer {
  space: ThickPath;
  ground: ThickPath;
  shown: Interval | null;
  path: DrawnPath | null;
}

export class Lane {
  readonly element: HTMLElement;
  readonly view: View;
  private readonly scene = new Scene();
  private readonly materials: ShaderMaterial[] = [];
  private readonly route: RouteLayer;
  private readonly baseline: RouteLayer;
  private readonly lostMaterial: ShaderMaterial;
  private readonly lostPosition = new BufferAttribute(new Float32Array(3), 3);
  private readonly lostPoint: Points;
  private readonly stateText: HTMLElement;
  private readonly news: { element: HTMLElement; t_s: number | null }[] = [];

  constructor(
    readonly mode: Mode,
    private readonly scenario: Scenario,
    private readonly data: CommonData,
    private readonly field: SatelliteField,
    private readonly sites: Float32Array,
    earth: Group,
    private readonly ends: { from: number; to: number },
  ) {
    this.element = document.createElement('article');
    this.element.className = 'lane';
    this.element.dataset.mode = mode;
    this.element.innerHTML = `
      <header class="lane-head">
        <h3></h3>
        <p class="lane-state"></p>
      </header>
      <div class="lane-globe"></div>
      <p class="lane-note"></p>
      <ul class="lane-news"></ul>`;
    (this.element.querySelector('h3') as HTMLElement).textContent = MODE_TITLES[mode];
    (this.element.querySelector('.lane-note') as HTMLElement).textContent = MODE_NOTES[mode];
    this.stateText = this.element.querySelector('.lane-state') as HTMLElement;
    this.buildNews(this.element.querySelector('.lane-news') as HTMLElement);

    this.scene.add(earth.clone());
    this.scene.add(this.linkLayer());
    this.scene.add(this.satelliteLayer());
    this.scene.add(this.siteLayer());
    this.baseline = this.routeLayer(THEME.baseline, 2, 0.35);
    this.route = this.routeLayer(THEME.delivered, 4.5, 1);
    this.lostMaterial = dotMaterial({ markColor: THEME.blackHole, markGrow: 1, downColor: THEME.failed });
    this.materials.push(this.lostMaterial);
    const lostColor = new Color(THEME.blackHole);
    this.lostPoint = dots(
      dotGeometry(this.lostPosition, new Float32Array([lostColor.r, lostColor.g, lostColor.b]), new Float32Array([11])),
      this.lostMaterial,
    );
    this.lostPoint.renderOrder = 3;
    this.scene.add(this.lostPoint);

    this.view = {
      element: this.element.querySelector('.lane-globe') as HTMLElement,
      scene: this.scene,
      beforeRender: (_w, h, ratio) => {
        for (const layer of [this.route, this.baseline]) {
          layer.space.material.resolution.set(_w, h);
          layer.ground.material.resolution.set(_w, h);
        }
        for (const m of this.materials) (m.uniforms.uScale as { value: number }).value = ratio * this.zoom;
      },
    };
  }

  /** Free the lane's GPU objects (shared ones are uploaded again when next drawn). */
  dispose(): void {
    this.scene.traverse((o) => {
      // LineSegments2 is a Mesh.
      if (o instanceof Mesh || o instanceof Points || o instanceof LineSegments) {
        const { geometry, material } = o as { geometry: BufferGeometry; material: Material | Material[] };
        geometry.dispose();
        for (const m of Array.isArray(material) ? material : [material]) m.dispose();
      }
    });
    this.element.remove();
  }

  /** The camera's zoom factor for dot sizes, set by the story each frame. */
  zoom = 1;

  /** Show story time t. */
  update(t: number): void {
    for (const m of this.materials) (m.uniforms.uTime as { value: number }).value = t;
    const iv = intervalAt(this.scenario.lanes[this.mode], t);
    const base = intervalAt(this.scenario.lanes.baseline, t);
    this.place(this.baseline, base);
    this.place(this.route, iv);
    if (iv !== null && iv !== this.route.shown) this.showState(iv);
    this.route.shown = iv;
    this.baseline.shown = base;
    for (const n of this.news) n.element.classList.toggle('reached', n.t_s !== null && t >= n.t_s);
  }

  private showState(iv: Interval): void {
    this.element.dataset.state = iv.state;
    this.element.dataset.intervalStart = String(iv.start_s);
    this.route.space.material.color.set(STATE_COLORS[iv.state]);
    let text = STATE_TITLES[iv.state];
    if (iv.state === 'delivered') text += `, ${formatMs(iv.one_way_ms)} one way`;
    if (iv.state === 'black_hole' && iv.failed_element !== null) text += `: lost at ${this.elementName(iv)}`;
    this.stateText.textContent = text;
    this.stateText.dataset.state = iv.state;
  }

  private elementName(iv: Interval): string {
    const e = iv.failed_element;
    if (e === null) return '';
    const sat = (i: number): string => this.data.satellites.name[i] ?? `satellite ${i}`;
    switch (e.kind) {
      case 'satellite':
        return sat(e.sat);
      case 'link':
        return `the laser link ${sat(e.a)} to ${sat(e.b)}`;
      case 'gateway':
      case 'city':
        return this.data.sites[e.site]?.name ?? '';
    }
  }

  /** Rebuild a route's lines when its interval changes; move its satellite points every frame. */
  private place(layer: RouteLayer, iv: Interval | null): void {
    if (iv !== layer.shown) {
      layer.path = iv === null ? null : drawnPath(iv, this.ends.from, this.ends.to, this.scenario.home_pop);
      const ground: number[] = [];
      const g = layer.path?.ground ?? [];
      for (let k = 1; k < g.length; k++) {
        const arc = surfaceArc(this.site(g[k - 1] as number), this.site(g[k] as number), SITE_RADIUS);
        ground.push(...(k === 1 ? arc : arc.slice(3)));
      }
      layer.ground.set(ground);
    }
    const points: number[] = [];
    for (const node of layer.path?.space ?? []) {
      const p = this.nodePosition(node);
      if (p !== null) points.push(...p);
    }
    layer.space.set(points);
    if (layer === this.route) {
      const lost = layer.path?.lost ?? null;
      const p = lost === null ? null : this.nodePosition(lost);
      this.lostPoint.visible = p !== null;
      if (p !== null) {
        this.lostPosition.set(p);
        this.lostPosition.needsUpdate = true;
      }
    }
  }

  private site(i: number): number[] {
    return Array.from(this.sites.subarray(3 * i, 3 * i + 3));
  }

  private nodePosition(node: PathNode): number[] | null {
    if (node.kind === 'site') return this.site(node.site);
    if (!this.field.isDrawable(node.sat)) return null;
    return Array.from((this.field.position.array as Float32Array).subarray(3 * node.sat, 3 * node.sat + 3));
  }

  private routeLayer(color: string, width: number, opacity: number): RouteLayer {
    const layer: RouteLayer = {
      space: new ThickPath(color, width, opacity),
      ground: new ThickPath(color, Math.max(1.5, width * 0.5), opacity * 0.7),
      shown: null,
      path: null,
    };
    layer.space.object.renderOrder = 2;
    layer.ground.object.renderOrder = 2;
    this.scene.add(layer.space.object, layer.ground.object);
    return layer;
  }

  private linkLayer(): ReturnType<typeof linkSegments> {
    const lines = linkSegments(this.field.position, this.field.linkIndex, THEME.links, 0.1);
    lines.onBeforeRender = () => lines.geometry.setDrawRange(0, this.field.linkIndexCount);
    return lines;
  }

  /** Satellites by shell; failed ones turn red at the failure; news marks per mode. */
  private satelliteLayer(): Points {
    const n = this.field.count;
    const down = new Float32Array(n).fill(NEVER);
    for (const s of this.scenario.failed.satellites) down[s] = this.scenario.failure_s;
    const mark =
      this.mode === 'flooding'
        ? Float32Array.from(this.scenario.flood.satellite_first_learn_s, (t) => t ?? NEVER)
        : satelliteDetections(this.scenario.facts, n, NEVER);
    const geometry = dotGeometry(this.field.position, this.field.shellColors, new Float32Array(n).fill(SAT_SIZE), mark, down);
    geometry.setIndex(this.field.pointIndex);
    const material = dotMaterial({
      markColor: THEME.news,
      markGrow: this.mode === 'flooding' ? 1.25 : 2.6,
      downColor: THEME.failed,
    });
    this.materials.push(material);
    const points = dots(geometry, material);
    points.onBeforeRender = () => geometry.setDrawRange(0, this.field.pointCount);
    return points;
  }

  /** The two cities, the gateways and the sender's home PoP. */
  private siteLayer(): Points {
    const { sites } = this.data;
    const shown = sites
      .map((s, i) => ({ s, i }))
      .filter(({ s, i }) => s.kind === 'gateway' || i === this.ends.from || i === this.ends.to || i === this.scenario.home_pop);
    const position = new BufferAttribute(new Float32Array(shown.length * 3), 3);
    const colors = new Float32Array(shown.length * 3);
    const sizes = new Float32Array(shown.length);
    const mark = new Float32Array(shown.length).fill(NEVER);
    const down = new Float32Array(shown.length).fill(NEVER);
    const color = new Color();
    shown.forEach(({ s, i }, k) => {
      (position.array as Float32Array).set(this.site(i), 3 * k);
      color.set(s.kind === 'city' ? THEME.city : s.kind === 'pop' ? THEME.pop : THEME.gateway);
      colors.set([color.r, color.g, color.b], 3 * k);
      sizes[k] = s.kind === 'gateway' ? 3.2 : 7;
      if (this.scenario.failed.gateways.includes(i)) down[k] = this.scenario.failure_s;
      const learnt =
        this.mode === 'flooding'
          ? this.scenario.flood.site_first_learn_s[i] ?? null
          : this.siteDetection(i);
      if (learnt !== null) mark[k] = learnt;
    });
    const material = dotMaterial({ markColor: THEME.news, markGrow: 1.6, downColor: THEME.failed });
    this.materials.push(material);
    const points = dots(dotGeometry(position, colors, sizes, mark, down), material);
    points.renderOrder = 1;
    return points;
  }

  private siteDetection(site: number): number | null {
    let first: number | null = null;
    for (const f of this.scenario.facts) {
      for (const d of f.detectors) {
        if (d.kind !== 'satellite' && d.index === site && (first === null || d.t_s < first)) first = d.t_s;
      }
    }
    return first;
  }

  /** The lines under the lane's title: who learned what, when (lit once the clock passes). */
  private buildNews(list: HTMLElement): void {
    const s = this.scenario;
    const fact = firstKnownFact(s.facts);
    const add = (t_s: number | null, text: string, key: string): void => {
      const li = document.createElement('li');
      li.dataset.news = key;
      li.textContent = text;
      list.append(li);
      this.news.push({ element: li, t_s });
    };
    add(s.failure_s, `Failure at ${formatInstant(s.failure_s)}`, 'failure');
    if (fact === null) {
      add(null, 'Nobody detects the failure in this run', 'none');
      return;
    }
    const detected = firstDetection(fact);
    if (this.mode === 'flooding') {
      add(detected, `Detected at ${formatInstant(detected)}; the flood starts`, 'detected');
      add(
        fact.flood_last_s,
        `Flood reached ${formatCount(fact.flood_nodes)} nodes by ${formatInstant(fact.flood_last_s)}`,
        'flood',
      );
      return;
    }
    add(detected, `Detected at ${formatInstant(detected)}`, 'detected');
    const reports = fact.reports ?? 0;
    const hops = fact.report_hops === null ? '' : `, the first after ${fact.report_hops} ${fact.report_hops === 1 ? 'hop' : 'hops'}`;
    add(
      fact.planner_known_s,
      `Planner knew at ${formatInstant(fact.planner_known_s)} (${reports} ${reports === 1 ? 'report' : 'reports'}${hops})`,
      'planner-known',
    );
    const plan = newPlanAt(s.plans[this.mode], fact.planner_known_s);
    add(plan, plan === null ? 'No new plan within the run' : `New plan in effect at ${formatInstant(plan)}`, 'new-plan');
  }
}

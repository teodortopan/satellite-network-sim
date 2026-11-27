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
import type { View } from '../../globe/globe-view';
import { dotGeometry, dotMaterial, dots, NEVER, shapeArray, ThickPath } from '../../globe/layers';
import { PathPlacer, type PathNode } from '../../globe/paths';
import type { SatelliteField } from '../../globe/satellite-field';
import { formatCount, formatInstant, formatMs } from '../../format';
import { BASELINE_STYLE, LANE_STATE_STYLES, line, MARKS, SHAPE_CODE, THEME, type LineStyle } from '../../theme';
import { setSwatch } from '../../ui/marks';
import type { Interval, LaneState, Mode, Scenario } from './data';
import {
  drawnPath,
  firstDetection,
  firstKnownFact,
  intervalAt,
  newPlanAt,
  satelliteDetections,
  type DrawnPath,
} from './state';

const SAT_SIZE = 2.2;

/** The three ways of reacting (the simulator calls them planner, local repair, flooding). */
export const MODE_TITLES: Record<Mode, string> = {
  planner: 'Wait for the control centre',
  'local-repair': 'Detour on the spot',
  flooding: 'Spread the word',
};

/** The same, short enough for a chart's axis. */
export const MODE_SHORT: Record<Mode, string> = {
  planner: 'Control centre',
  'local-repair': 'On the spot',
  flooding: 'Spread the word',
};

export const MODE_NOTES: Record<Mode, string> = {
  planner: 'Only a control centre on the ground changes the paths. It waits until news of the break reaches it.',
  'local-repair': 'The satellite just before the break switches to a backup path it had ready. The control centre catches up later.',
  flooding: 'Each satellite passes the news to its neighbours, and every one works out new paths by itself.',
};

export const STATE_TITLES: Record<LaneState, string> = {
  delivered: 'Arriving',
  black_hole: 'Lost (black hole)',
  loop: 'Going in circles',
  no_route: 'No route',
};

/** How the first report reached the control centre: `hops` is how often it was passed on. */
export function relayed(hops: number): string {
  if (hops === 0) return 'the first sent straight there';
  return `the first passed on ${hops} ${hops === 1 ? 'time' : 'times'}`;
}

/** How much a failed satellite's cross grows over a working satellite's dot. */
const FAILED_GROW = 2.8;

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
  private readonly placer: PathPlacer;

  constructor(
    readonly mode: Mode,
    private readonly scenario: Scenario,
    private readonly data: CommonData,
    private readonly field: SatelliteField,
    sites: Float32Array,
    earth: Group,
    private readonly ends: { from: number; to: number },
  ) {
    this.placer = new PathPlacer(field, sites);
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
    this.scene.add(field.segments(THEME.links, 0.1));
    this.scene.add(this.satelliteLayer());
    this.scene.add(this.siteLayer());
    this.baseline = this.routeLayer(BASELINE_STYLE, 0.35);
    this.route = this.routeLayer(LANE_STATE_STYLES.delivered as LineStyle, 1);
    this.lostMaterial = dotMaterial({ markColor: MARKS.blackHole.color, markGrow: 1, downColor: MARKS.failed.color });
    this.materials.push(this.lostMaterial);
    const lostColor = new Color(MARKS.blackHole.color);
    this.lostPoint = dots(
      dotGeometry(
        this.lostPosition,
        new Float32Array([lostColor.r, lostColor.g, lostColor.b]),
        new Float32Array([15]),
        undefined,
        undefined,
        shapeArray([MARKS.blackHole.shape]),
      ),
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
    const style = LANE_STATE_STYLES[iv.state] ?? null;
    if (style !== null) {
      this.route.space.setStyle(style);
      this.route.space.layDashes();
    }
    let text = STATE_TITLES[iv.state];
    if (iv.state === 'delivered') text += `, ${formatMs(iv.one_way_ms)} one way`;
    if (iv.state === 'black_hole' && iv.failed_element !== null) text += ` at ${this.elementName(iv)}`;
    this.stateText.textContent = text;
    setSwatch(this.stateText, style === null ? null : line(style));
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
        return `the laser link between ${sat(e.a)} and ${sat(e.b)}`;
      case 'gateway':
      case 'city':
        return this.data.sites[e.site]?.name ?? '';
    }
  }

  /** Rebuild a route's lines when its interval changes; move its satellite points every frame. */
  private place(layer: RouteLayer, iv: Interval | null): void {
    const changed = iv !== layer.shown;
    if (changed) {
      layer.path = iv === null ? null : drawnPath(iv, this.ends.from, this.ends.to, this.scenario.home_pop);
      layer.ground.set(this.placer.ground(layer.path?.ground ?? []));
    }
    layer.space.set(this.placer.points(layer.path?.space ?? []));
    if (changed) layer.space.layDashes();
    if (layer === this.route) {
      const lost: PathNode | null = layer.path?.lost ?? null;
      const p = lost === null ? null : this.placer.node(lost);
      this.lostPoint.visible = p !== null;
      if (p !== null) {
        this.lostPosition.set(p);
        this.lostPosition.needsUpdate = true;
      }
    }
  }

  private routeLayer(style: LineStyle, opacity: number): RouteLayer {
    const layer: RouteLayer = {
      space: new ThickPath(style.color, style.width, opacity),
      ground: new ThickPath(style.color, Math.max(1.5, style.width * 0.5), opacity * 0.7),
      shown: null,
      path: null,
    };
    layer.space.object.renderOrder = 2;
    layer.ground.object.renderOrder = 2;
    this.scene.add(layer.space.object, layer.ground.object);
    return layer;
  }

  /** Satellites as dots by shell; failed ones turn into crosses at the failure; those with the
   * news (per mode) into rings. */
  private satelliteLayer(): Points {
    const n = this.field.count;
    const down = new Float32Array(n).fill(NEVER);
    for (const s of this.scenario.failed.satellites) down[s] = this.scenario.failure_s;
    const mark =
      this.mode === 'flooding'
        ? Float32Array.from(this.scenario.flood.satellite_first_learn_s, (t) => t ?? NEVER)
        : satelliteDetections(this.scenario.facts, n, NEVER);
    const material = dotMaterial({
      markColor: MARKS.news.color,
      markGrow: this.mode === 'flooding' ? 1.6 : 2.8,
      markShape: MARKS.news.shape,
      downColor: MARKS.failed.color,
      downGrow: FAILED_GROW,
      downShape: MARKS.failed.shape,
    });
    this.materials.push(material);
    return this.field.points(this.field.shellColors, new Float32Array(n).fill(SAT_SIZE), material, mark, down);
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
    const shapes = new Float32Array(shown.length);
    const color = new Color();
    shown.forEach(({ s, i }, k) => {
      (position.array as Float32Array).set(this.placer.site(i), 3 * k);
      const site = s.kind === 'city' ? MARKS.city : s.kind === 'pop' ? MARKS.pop : MARKS.gateway;
      color.set(site.color);
      colors.set([color.r, color.g, color.b], 3 * k);
      shapes[k] = SHAPE_CODE[site.shape];
      sizes[k] = s.kind === 'gateway' ? 4.5 : 9;
      if (this.scenario.failed.gateways.includes(i)) down[k] = this.scenario.failure_s;
      const learnt =
        this.mode === 'flooding'
          ? this.scenario.flood.site_first_learn_s[i] ?? null
          : this.siteDetection(i);
      if (learnt !== null) mark[k] = learnt;
    });
    const material = dotMaterial({
      markColor: MARKS.news.color,
      markGrow: 1.6,
      markShape: MARKS.news.shape,
      downColor: MARKS.failed.color,
      downGrow: 2.2,
      downShape: MARKS.failed.shape,
    });
    this.materials.push(material);
    const points = dots(dotGeometry(position, colors, sizes, mark, down, shapes), material);
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
    add(s.failure_s, `Breaks at ${formatInstant(s.failure_s)}`, 'failure');
    if (fact === null) {
      add(null, 'Nobody notices the break in this run', 'none');
      return;
    }
    const detected = firstDetection(fact);
    if (this.mode === 'flooding') {
      add(detected, `Noticed at ${formatInstant(detected)}; the news starts to spread`, 'detected');
      add(
        fact.flood_last_s,
        `News reached ${formatCount(fact.flood_nodes)} satellites and stations by ${formatInstant(fact.flood_last_s)}`,
        'flood',
      );
      return;
    }
    add(detected, `Noticed at ${formatInstant(detected)}`, 'detected');
    const reports = fact.reports ?? 0;
    const hops =
      fact.report_hops === null ? '' : `, ${relayed(fact.report_hops)}`;
    add(
      fact.planner_known_s,
      `Control centre knows at ${formatInstant(fact.planner_known_s)} (${reports} ${reports === 1 ? 'report' : 'reports'}${hops})`,
      'planner-known',
    );
    const plan = newPlanAt(s.plans[this.mode], fact.planner_known_s);
    add(plan, plan === null ? 'No new paths within the run' : `New paths in use at ${formatInstant(plan)}`, 'new-plan');
  }
}

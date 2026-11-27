// Story: the constellation (spec 4.7). Every satellite over the window marked by shell, one
// shell or one plane picked out, the planned laser links of one pattern, the ground sites by
// kind, and the constellation program's numbers per shell and per pattern.

import type { BufferAttribute } from 'three';
import { Clock } from '../../clock/clock';
import { ClockView } from '../../clock/clock-view';
import { parseLinks, type Links, type SiteKind } from '../../data/common';
import { formatCount } from '../../format';
import { siteScenePositions } from '../../globe/paths';
import { SatelliteField } from '../../globe/satellite-field';
import { rgbArray, Stage } from '../../globe/stage';
import { dot, FADED, MARKS, SHAPE_CODE, shellMark, THEME, type DotMark } from '../../theme';
import { shapeArray } from '../../globe/layers';
import { setSwatch } from '../../ui/marks';
import { dropdown, frameLoop, segmented, toggle } from '../../ui/controls';
import type { Story, StoryContext } from '../story';
import { parseConstellationStory, type ConstellationStory } from './data';
import { chosen, linksOf } from './state';

const START_SPEED = 60;
const CAMERA_DISTANCE = 4.3;
const SAT_SIZE = 3.6;
const FADED_SIZE = 1.6;
/** The planned laser links: one pattern at a time, so one grey (the menu names the pattern). */
const LINK_OPACITY = 0.2;

const SITE_KINDS: { kind: SiteKind; text: string; mark: DotMark; size: number }[] = [
  { kind: 'city', text: 'Cities', mark: MARKS.city, size: 11 },
  { kind: 'gateway', text: 'Gateways', mark: MARKS.gateway, size: 7 },
  { kind: 'pop', text: 'PoPs', mark: MARKS.pop, size: 7 },
];

export const constellationStory: Story = {
  id: 'constellation',
  title: 'The constellation',
  mount,
};

async function mount({ data, root }: StoryContext): Promise<() => void> {
  const { manifest } = data.source;
  const story = await data.source.json('story-constellation.json', (v, c) => parseConstellationStory(v, c, data.satellites));
  const links = await Promise.all(
    story.patterns.map((p) => data.source.json(p.file, (v, c) => parseLinks(v, c, manifest.satellites))),
  );

  root.innerHTML = `
    <section class="story constellation">
      <div class="intro">
        <h1>The constellation</h1>
        <div class="intro-text">
          <p class="lede"></p>
          <p class="how"></p>
        </div>
      </div>
      <div class="control-bar"></div>
      <div class="stage">
        <div class="stage-globe"></div>
        <aside class="stage-side">
          <h2>On the globe</h2>
          <ul class="legend-list"></ul>
          <p class="stage-note links-note"></p>
        </aside>
      </div>
      <div class="clock-dock"></div>
      <div class="panels">
        <section class="panel shells-panel">
          <h2>Shells</h2>
          <p class="panel-note shells-note"></p>
          <div class="table-scroll"></div>
        </section>
        <section class="panel patterns-panel">
          <h2>Laser patterns</h2>
          <p class="panel-note patterns-note"></p>
          <div class="table-scroll"></div>
        </section>
      </div>
    </section>`;
  const el = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const section = el('.story');
  writeText(el('.lede'), el('.how'), story, manifest.satellites);
  el('.shells-note').textContent =
    `From the constellation program’s summary (${story.sources.shells}) at the plan time ${story.plan_time_utc.replace('T', ' ')} UTC. ` +
    'A shell is a height band and an inclination; a plane is a group of satellites on one orbit plane.';
  el('.patterns-note').textContent =
    `From the topology program’s summaries (${story.sources.patterns.join(', ')}). In-plane links join neighbours ` +
    'on one orbit; cross-plane links join satellites of neighbouring planes.';
  el('.shells-panel .table-scroll').append(shellTable(story));
  el('.patterns-panel .table-scroll').append(patternTable(story));

  // The globe.
  const globeEl = el('.stage-globe');
  const stage = new Stage(globeEl);
  const capacity = Math.max(...links.map((l) => l.pairs.length));
  const field = new SatelliteField(data, (links[0] as Links).pairs, capacity);
  const segments = field.segments(THEME.links, LINK_OPACITY);
  stage.add(segments);
  const colors = new Float32Array(field.count * 3);
  const sizes = new Float32Array(field.count);
  const ownShapes = shapeArray(data.satellites.shell.map((s) => shellMark(s).shape));
  const shapes = ownShapes.slice();
  const satPoints = field.points(colors, sizes, stage.dotMaterial(), undefined, undefined, shapes);
  stage.add(satPoints);
  const sitePositions = siteScenePositions(data);
  const siteLayers = SITE_KINDS.map(({ kind, mark, size }) => {
    const idx = data.sites.flatMap((s, i) => (s.kind === kind ? [i] : []));
    const layer = stage.fixedDots(
      idx.flatMap((i) => Array.from(sitePositions.subarray(3 * i, 3 * i + 3))),
      idx.map(() => mark.color),
      idx.map(() => size),
      idx.map(() => mark.shape),
    );
    stage.add(layer);
    return { kind, layer, count: idx.length };
  });
  stage.globe.lookFrom([1, 0.75, -0.4], CAMERA_DISTANCE);

  // The choice: shell, plane, pattern.
  let shell: number | null = null;
  let plane: number | null = null;
  let patternIndex = Math.max(story.patterns.findIndex((p) => p.pattern === 'three'), 0);
  let drawnLinks = links[patternIndex] as Links;
  let fresh = true;
  const apply = (): void => {
    const picked = chosen(data.satellites, shell, plane);
    const c = rgbArray(data.satellites.shell.map((s, i) => (picked[i] === 1 ? shellMark(s).color : FADED.color)));
    colors.set(c);
    picked.forEach((p, i) => {
      sizes[i] = p === 1 ? SAT_SIZE * (shellMark(data.satellites.shell[i] ?? 0).grow ?? 1) : FADED_SIZE;
      shapes[i] = p === 1 ? (ownShapes[i] as number) : SHAPE_CODE[FADED.shape];
    });
    (satPoints.geometry.getAttribute('aColor') as BufferAttribute).needsUpdate = true;
    (satPoints.geometry.getAttribute('aSize') as BufferAttribute).needsUpdate = true;
    (satPoints.geometry.getAttribute('aShape') as BufferAttribute).needsUpdate = true;
    drawnLinks = links[patternIndex] as Links;
    field.setLinks(linksOf(drawnLinks.pairs, picked, plane !== null));
    section.dataset.shell = shell === null ? 'all' : (data.satellites.shells[shell] ?? '');
    section.dataset.plane = plane === null ? 'all' : String(plane);
    section.dataset.pattern = story.patterns[patternIndex]?.pattern ?? '';
    globeEl.dataset.satellitesChosen = String(picked.reduce((n, p) => n + p, 0));
    writeLegend(el('.legend-list'), shell, data.satellites.shells);
    el('.links-note').textContent =
      `Lines: the planned laser links of the pattern “${drawnLinks.pattern}” at the plan time` +
      `${plane === null ? '' : ', those of the chosen plane and those that leave it'}. ` +
      'Planned links, not the links that work: a planned link can be out of sight at a moment.';
    fresh = true;
  };

  const bar = el('.control-bar');
  const planeHost = document.createElement('div');
  const showPlanes = (): void => {
    planeHost.replaceChildren();
    if (shell === null) return;
    const count = story.shells[shell]?.planes ?? 0;
    planeHost.append(
      dropdown(
        'Plane',
        [{ value: 'all', text: 'All planes' }, ...Array.from({ length: count }, (_, i) => ({ value: String(i), text: `Plane ${i}` }))],
        (i) => {
          plane = i === 0 ? null : i - 1;
          apply();
        },
      ),
    );
  };
  bar.append(
    dropdown('Shell', [{ value: 'all', text: 'All shells' }, ...data.satellites.shells.map((s) => ({ value: s, text: `Shell ${s}` }))], (i) => {
      shell = i === 0 ? null : i - 1;
      plane = null;
      showPlanes();
      apply();
    }),
    planeHost,
  );
  const patternMenu = segmented(
    'Laser pattern',
    story.patterns.map((p) => ({ value: p.pattern, text: p.pattern })),
    (i) => {
      patternIndex = i;
      apply();
    },
  );
  patternMenu.select(patternIndex);
  const siteToggles = document.createElement('div');
  siteToggles.className = 'toggles';
  siteToggles.setAttribute('role', 'group');
  siteToggles.setAttribute('aria-label', 'Ground sites');
  for (const { kind, text, mark } of SITE_KINDS) {
    const layer = siteLayers.find((l) => l.kind === kind);
    siteToggles.append(
      toggle({ value: kind, text: `${text} (${formatCount(layer?.count ?? 0)})` }, true, dot(mark), (on) => {
        if (layer !== undefined) layer.layer.visible = on;
        globeEl.dataset.sites = siteLayers.filter((l) => l.layer.visible).map((l) => l.kind).join(' ');
      }),
    );
  }
  globeEl.dataset.sites = siteLayers.map((l) => l.kind).join(' ');
  bar.append(patternMenu.element, siteToggles);
  apply();

  const clock = new Clock({ duration_s: manifest.settings.window_s, speed: START_SPEED });
  const clockView = new ClockView(clock, { marks: [], spans: [], holdLabel: '' });
  el('.clock-dock').append(clockView.element);

  let drawn_t = Number.NaN;
  const stop = frameLoop((real_s) => {
    clock.tick(real_s);
    if (clock.t !== drawn_t || fresh) {
      drawn_t = clock.t;
      field.update(clock.t);
      // What is drawn, read back from the drawn objects for the browser test.
      globeEl.dataset.drawnPattern = drawnLinks.pattern;
      globeEl.dataset.linksDrawn = String(field.linkIndexCount / 2);
      fresh = false;
    }
    clockView.update();
    stage.render();
    document.body.dataset.ready = 'true';
  });

  return () => {
    stop();
    clockView.dispose();
    stage.dispose();
    root.replaceChildren();
  };
}

function writeLegend(list: HTMLElement, shell: number | null, shells: readonly string[]): void {
  list.replaceChildren(
    ...shells.map((name, i) => {
      const li = document.createElement('li');
      li.textContent = `Shell ${name}`;
      setSwatch(li, dot(shell === null || shell === i ? shellMark(i) : FADED));
      return li;
    }),
  );
}

function cell(tr: HTMLElement, text: string, data: Record<string, string>, header = false): void {
  const td = document.createElement(header ? 'th' : 'td');
  if (header) td.setAttribute('scope', 'row');
  Object.assign(td.dataset, data);
  td.textContent = text;
  tr.append(td);
}

function table(head: readonly string[]): { table: HTMLTableElement; body: HTMLElement } {
  const t = document.createElement('table');
  const tr = document.createElement('tr');
  for (const h of head) {
    const th = document.createElement('th');
    th.scope = 'col';
    th.textContent = h;
    tr.append(th);
  }
  const thead = document.createElement('thead');
  thead.append(tr);
  const body = document.createElement('tbody');
  t.append(thead, body);
  return { table: t, body };
}

function shellTable(story: ConstellationStory): HTMLTableElement {
  const { table: t, body } = table(['Shell', 'Satellites', 'Planes', 'Height', 'Inclination', 'Plane size']);
  story.shells.forEach((s, i) => {
    const tr = document.createElement('tr');
    cell(tr, s.shell, { shell: s.shell }, true);
    setSwatch(tr.firstElementChild as HTMLElement, dot(shellMark(i)));
    cell(tr, formatCount(s.satellites_in_planes), { shell: s.shell, field: 'satellites_in_planes' });
    cell(tr, formatCount(s.planes), { shell: s.shell, field: 'planes' });
    cell(tr, `${s.height_min_km} to ${s.height_max_km} km`, { shell: s.shell, field: 'height' });
    cell(tr, `${s.inclination_deg}° ± ${s.inclination_tolerance_deg}°`, { shell: s.shell, field: 'inclination' });
    cell(tr, `${s.plane_size_min} to ${s.plane_size_max}, median ${s.plane_size_median}`, { shell: s.shell, field: 'plane_size' });
    body.append(tr);
  });
  return t;
}

function patternTable(story: ConstellationStory): HTMLTableElement {
  const { table: t, body } = table(['Pattern', 'Links', 'In plane', 'Cross plane', 'Second cross plane']);
  for (const p of story.patterns) {
    const tr = document.createElement('tr');
    cell(tr, p.pattern, { pattern: p.pattern }, true);
    cell(tr, formatCount(p.links), { pattern: p.pattern, field: 'links' });
    cell(tr, formatCount(p.by_kind.in_plane), { pattern: p.pattern, field: 'in_plane' });
    cell(tr, formatCount(p.by_kind.cross_plane), { pattern: p.pattern, field: 'cross_plane' });
    cell(tr, formatCount(p.by_kind.second_cross_plane), { pattern: p.pattern, field: 'second_cross_plane' });
    body.append(tr);
  }
  return t;
}

function writeText(lede: HTMLElement, how: HTMLElement, story: ConstellationStory, satellites: number): void {
  const largest = story.shells.reduce((a, b) => (b.satellites_in_planes > a.satellites_in_planes ? b : a));
  const patterns = story.patterns.map((p) => `“${p.pattern}” ${formatCount(p.links)}`).join(', ');
  lede.textContent =
    `These are the ${formatCount(satellites)} satellites of the snapshot that the engine places in a shell and a plane. ` +
    'A shell is a group flying at about the same height and the same tilt to the equator (the inclination); ' +
    `there are ${formatCount(story.shells.length)}, the largest, ${largest.shell}, with ${formatCount(largest.satellites_in_planes)} satellites in ` +
    `${formatCount(largest.planes)} planes. A laser pattern is the rule that decides which satellites point their lasers at each other. ` +
    `The number of planned laser links per pattern: ${patterns}.`;
  how.textContent =
    'Each dot is a satellite; its shape and brightness say its shell (the list beside the globe shows each shell’s mark). ' +
    'Pick one shell or one plane to see it alone. The faint lines are one pattern’s planned laser links; lasers never join ' +
    'two shells. Ringed dots with a centre are the cities of the study pairs, hollow squares the gateways (ground stations ' +
    'that join the satellites to the Internet), diamonds the PoPs (points of presence, where the operator’s network meets ' +
    'the rest of the Internet). Play the clock to see the satellites move; drag to turn the globe.';
}

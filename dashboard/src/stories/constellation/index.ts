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
import { dot, FADED, line, MARKS, SHAPE_CODE, shellMark, THEME, type DotMark } from '../../theme';
import { fillFold, introMarkup, legendList } from '../../ui/intro';
import { patternShort, patternWords, shellName } from '../../ui/words';
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

const SITE_KINDS: { kind: SiteKind; text: string; one: string; mark: DotMark; size: number }[] = [
  { kind: 'city', text: 'Cities', one: 'City', mark: MARKS.city, size: 11 },
  { kind: 'gateway', text: 'Ground stations', one: 'Ground station', mark: MARKS.gateway, size: 7 },
  { kind: 'pop', text: 'Internet exchanges', one: 'Internet exchange', mark: MARKS.pop, size: 7 },
];

export const constellationStory: Story = {
  id: 'constellation',
  title: 'The satellites',
  summary: 'Every satellite in the network, how they are grouped, and how lasers connect them.',
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
      ${introMarkup('The satellites')}
      <div class="control-bar"></div>
      <div class="stage">
        <div class="stage-globe"></div>
        <aside class="stage-side">
          <h2>On the globe</h2>
          <ul class="legend-list globe-legend"></ul>
          <p class="stage-note links-note"></p>
        </aside>
      </div>
      <div class="clock-dock"></div>
      <div class="panels">
        <section class="panel shells-panel">
          <h2>The groups of satellites (shells)</h2>
          <p class="panel-note shells-note"></p>
          <div class="table-scroll"></div>
        </section>
        <section class="panel patterns-panel">
          <h2>Laser layouts</h2>
          <p class="panel-note patterns-note"></p>
          <div class="table-scroll"></div>
        </section>
      </div>
    </section>`;
  const el = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const section = el('.story');
  writeText(root, story, manifest.satellites, manifest.snapshot);
  el('.shells-note').textContent =
    'Each group flies at about the same height and the same tilt to the equator; within a group, an orbit is ' +
    'one ring of satellites following each other.';
  el('.patterns-note').textContent =
    'How many laser links each layout plans, and where they point: along the orbit (to the satellites ahead and ' +
    'behind), to the next orbit over, or to the orbit two over.';
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
    writeLegend(el('.stage-side .legend-list'), shell, data.satellites.shells);
    el('.links-note').textContent =
      `The lines are the planned laser links, ${patternShort(drawnLinks.pattern)}` +
      `${plane === null ? '' : ', of the chosen orbit and those that leave it'}. ` +
      'A planned link can be out of sight for a moment.';
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
        'Orbit (plane)',
        [{ value: 'all', text: 'All orbits' }, ...Array.from({ length: count }, (_, i) => ({ value: String(i), text: `Orbit ${i}` }))],
        (i) => {
          plane = i === 0 ? null : i - 1;
          apply();
        },
      ),
    );
  };
  bar.append(
    dropdown('Group (shell)', [{ value: 'all', text: 'All groups' }, ...data.satellites.shells.map((s) => ({ value: s, text: shellName(s) }))], (i) => {
      shell = i === 0 ? null : i - 1;
      plane = null;
      showPlanes();
      apply();
    }),
    planeHost,
  );
  const patternMenu = segmented(
    'Laser pattern',
    story.patterns.map((p) => ({ value: p.pattern, text: patternShort(p.pattern) })),
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
  const patternGroup = document.createElement('div');
  patternGroup.className = 'control-group';
  patternGroup.innerHTML = '<span class="control-label">Laser layout</span>';
  patternGroup.append(patternMenu.element);
  bar.append(patternGroup, siteToggles);
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
      li.textContent = shellName(name);
      setSwatch(li, dot(shell === null || shell === i ? shellMark(i) : FADED));
      return li;
    }),
    ...legendList([
      [line({ color: THEME.links, width: 1, dash: [] }, 0.6), 'Laser link'],
      ...SITE_KINDS.map(({ one, mark }) => [dot(mark), one] as const),
    ]).children,
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
  const { table: t, body } = table(['Group', 'Satellites', 'Orbits', 'Height', 'Tilt to the equator', 'Satellites per orbit']);
  story.shells.forEach((s, i) => {
    const tr = document.createElement('tr');
    cell(tr, shellName(s.shell), { shell: s.shell }, true);
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
  const { table: t, body } = table(['Layout', 'Laser links', 'Along the orbit', 'To the next orbit', 'To the orbit two over']);
  for (const p of story.patterns) {
    const tr = document.createElement('tr');
    cell(tr, `${patternShort(p.pattern)} (“${p.pattern}”)`, { pattern: p.pattern }, true);
    cell(tr, formatCount(p.links), { pattern: p.pattern, field: 'links' });
    cell(tr, formatCount(p.by_kind.in_plane), { pattern: p.pattern, field: 'in_plane' });
    cell(tr, formatCount(p.by_kind.cross_plane), { pattern: p.pattern, field: 'cross_plane' });
    cell(tr, formatCount(p.by_kind.second_cross_plane), { pattern: p.pattern, field: 'second_cross_plane' });
    body.append(tr);
  }
  return t;
}

function writeText(root: HTMLElement, story: ConstellationStory, satellites: number, snapshot: string): void {
  const q = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  q('.lede').textContent =
    `Each dot is one of ${formatCount(satellites)} real Starlink satellites, where it was on ${snapshot}. They fly in ` +
    'groups at about the same height and the same tilt to the equator (shells), each group in rings that share one ' +
    'orbit (planes). The faint lines are the laser links that connect the satellites to each other.';
  q('.try-text').textContent =
    'Press Play to watch them circle the Earth. Pick one group to see it alone, then one orbit. Switch the laser ' +
    'layout to see how many partners each satellite gets.';
  fillFold(q('.how-body'), [
    'Each group of satellites has its own dot shape and brightness, listed beside the globe. A group not chosen fades to small dark dots.',
    legendList(
      [
        [dot(MARKS.city), 'A city used in the other stories.'],
        [dot(MARKS.gateway), 'A ground station (gateway): where the satellites hand messages to the Internet on the ground.'],
        [dot(MARKS.pop), 'An Internet exchange (point of presence, PoP): where the satellite operator’s network meets the rest of the Internet.'],
      ],
      'legend-list fold-legend',
    ),
    'Lasers never join two groups. Drag the globe to turn it; scroll or pinch to zoom.',
  ]);
  const largest = story.shells.reduce((a, b) => (b.satellites_in_planes > a.satellites_in_planes ? b : a));
  const patterns = story.patterns.map((p) => `${formatCount(p.links)} with ${patternShort(p.pattern)}`).join(', ');
  fillFold(q('.sources-body'), [
    `There are ${formatCount(story.shells.length)} groups; the largest, ${shellName(largest.shell)}, has ` +
      `${formatCount(largest.satellites_in_planes)} satellites in ${formatCount(largest.planes)} orbits. Planned laser links: ${patterns}.`,
    `Laser layouts: ${story.patterns.map((p) => patternWords(p.pattern)).join('; ')}.`,
    `Groups and orbits are found from the satellites’ positions at ${story.plan_time_utc.replace('T', ' ')} UTC (world time). ` +
      `Computed by the simulator’s constellation program (${story.sources.shells}) and topology program ` +
      `(${story.sources.patterns.join(', ')}).`,
  ]);
}

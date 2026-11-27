// Story: the network falling apart (spec 4.8). Charts from Phase 4's committed summaries
// (pairs still connected, and the largest connected part, against the failed share), and the
// real constellation at one instant with a share of its satellites failed.

import { formatCount, formatMs, formatShare } from '../../format';
import { lineChart, type Line, type ResponsivePlot } from '../../charts/plot';
import { parseLinks, type CommonData, type Links } from '../../data/common';
import type { ThickPath } from '../../globe/layers';
import { PathPlacer, routeNodes, siteScenePositions } from '../../globe/paths';
import { SatelliteField } from '../../globe/satellite-field';
import { rgbArray, Stage } from '../../globe/stage';
import { BASELINE_STYLE, dot, failureKindStyle, line, MARKS, patternStyle, shellColor, THEME, type DotShape, type LineStyle, type Mark } from '../../theme';
import { shapeArray } from '../../globe/layers';
import { setSwatch } from '../../ui/marks';
import { frameLoop, segmented } from '../../ui/controls';
import { fillFold, introMarkup } from '../../ui/intro';
import { patternShort, shellName } from '../../ui/words';
import type { Story, StoryContext } from '../story';
import { cellOf, parseResilienceStory, type Cell, type ResilienceStory, type Step, type Table } from './data';
import { afterRecovery, alignTo, pickLinks, pointsOf, type Points } from './state';

const PATTERNS = ['plus-grid', 'three', 'six'] as const;
const CAMERA_DISTANCE = 4.2;
/** Failed satellites: crosses, dimmer than the failure mark of the other stories because a
 * share of up to 30% of the constellation is drawn. */
const FAILED = { color: '#9c9c9c', shape: 'cross' } as const;
const FAILED_SIZE = 3.4;
const WORKING_SIZE = 1.8;
/** The route after recovery, and the route with nothing failed (faint, thin) beneath it. */
const ROUTE_STYLE: LineStyle = { color: THEME.delivered, width: 3.6, dash: [] };
const BASELINE_OPACITY = 0.35;
/** Dashed: re-pointed (what fails), or lasers fail (percolation); the grey and the dot on the
 * points say the kind of failure or the pattern. */
const DASH = [7, 5];
const LINK_OPACITY = 0.3;
const SHARE_TICKS = [0.001, 0.003, 0.01, 0.03, 0.1, 0.3];

export const resilienceStory: Story = {
  id: 'resilience',
  title: 'When many things break',
  summary: 'Break more and more of the network at once and see how many cities can still reach each other.',
  mount,
};

interface LineDef {
  label: string;
  color: string;
  marker: DotShape;
  dash?: number[];
  match: Record<string, Cell>;
}

/** A chart line in a series' style (its grey and its dot), dashed or not. */
function styled(style: LineStyle, dashed = false): Pick<LineDef, 'color' | 'marker' | 'dash'> {
  return { color: style.color, marker: style.marker ?? 'disc', ...(dashed ? { dash: DASH } : {}) };
}

const WHAT_FAILS: LineDef[] = [
  { label: 'Random satellites break', ...styled(failureKindStyle('random-satellites')), match: { group: 'random-satellites', scenario: 'random-satellites', pattern: 'three', repoint: 'off' } },
  { label: 'Random laser links break', ...styled(failureKindStyle('random-lasers')), match: { group: 'random-lasers', scenario: 'random-lasers', pattern: 'three', repoint: 'off' } },
  { label: 'The busiest satellites break', ...styled(failureKindStyle('attack')), match: { group: 'attack', scenario: 'attack', pattern: 'three', repoint: 'off' } },
  { label: 'Random satellites, lasers re-aimed', ...styled(failureKindStyle('random-satellites'), true), match: { group: 'recovery-repointing', scenario: 'random-satellites', pattern: 'three', repoint: 'on' } },
  { label: 'Busiest satellites, lasers re-aimed', ...styled(failureKindStyle('attack'), true), match: { group: 'recovery-repointing', scenario: 'attack', pattern: 'three', repoint: 'on' } },
];

const BY_PATTERN: LineDef[] = [
  { label: patternShort('plus-grid'), ...styled(patternStyle('plus-grid')), match: { group: 'pattern', scenario: 'random-satellites', pattern: 'plus-grid', repoint: 'off' } },
  { label: patternShort('three'), ...styled(patternStyle('three')), match: { group: 'random-satellites', scenario: 'random-satellites', pattern: 'three', repoint: 'off' } },
  { label: patternShort('six'), ...styled(patternStyle('six')), match: { group: 'pattern', scenario: 'random-satellites', pattern: 'six', repoint: 'off' } },
];

const PERCOLATION: LineDef[] = [
  { label: `${patternShort('plus-grid')}, satellites break`, ...styled(patternStyle('plus-grid')), match: { links: 'plus-grid', kind: 'satellites' } },
  { label: `${patternShort('plus-grid')}, lasers break`, ...styled(patternStyle('plus-grid'), true), match: { links: 'plus-grid', kind: 'laser-links' } },
  { label: `${patternShort('three')}, satellites break`, ...styled(patternStyle('three')), match: { links: 'three', kind: 'satellites' } },
  { label: `${patternShort('three')}, lasers break`, ...styled(patternStyle('three'), true), match: { links: 'three', kind: 'laser-links' } },
];

async function mount({ data, root }: StoryContext): Promise<() => void> {
  const { manifest } = data.source;
  const links = new Map<string, Links>();
  for (const l of await Promise.all(PATTERNS.map((p) => data.source.json(`links-${p}.json`, (v, c) => parseLinks(v, c, manifest.satellites))))) {
    links.set(l.pattern, l);
  }
  const story = await data.source.json('story-resilience.json', (v, c) =>
    parseResilienceStory(v, c, manifest.satellites, data.sites, manifest.settings.window_s, (p) => (links.get(p)?.pairs.length ?? 0) / 2),
  );

  root.innerHTML = `
    <section class="story resilience">
      ${introMarkup('When many things break')}
      <div class="instant">
        <div class="control-bar"><div class="control-group"><span class="control-label">How much breaks</span></div></div>
        <div class="stage">
          <div class="stage-globe"></div>
          <aside class="stage-side">
            <h2>City pairs after the break</h2>
            <p class="instant-text"></p>
            <ul class="legend-list globe-legend resilience-legend"></ul>
            <ul class="pair-list"></ul>
          </aside>
        </div>
      </div>
      <div class="charts-head">
        <h2>As more and more breaks</h2>
        <p>Each chart goes from a little broken (left) to a lot broken (right). The example on the globe above is one point on these lines.</p>
      </div>
      <div class="panels charts">
        <section class="panel" data-chart="what-fails">
          <h2>What breaks</h2>
          <p class="panel-note"></p>
          <div class="line-plot"></div>
        </section>
        <section class="panel" data-chart="pattern">
          <h2>More lasers per satellite</h2>
          <p class="panel-note"></p>
          <div class="line-plot"></div>
        </section>
        <section class="panel" data-chart="percolation">
          <h2>When the network falls apart</h2>
          <p class="panel-note"></p>
          <div class="line-plot"></div>
        </section>
        <details class="panel numbers">
          <summary>The numbers behind these charts</summary>
          <div class="numbers-tables"></div>
        </details>
      </div>
      <div class="panels">
        <section class="panel network-panel">
          <h2>How the network split, in the example on the globe</h2>
          <p class="panel-note network-note"></p>
          <div class="table-scroll"></div>
        </section>
      </div>
    </section>`;
  const el = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const section = el('.story');
  writeText(root, story);

  const recovery = story.charts.recovery_summary;
  const percolation = story.charts.percolation;
  const plots: ResponsivePlot[] = [];
  const note = (chart: string, text: string): void => {
    (root.querySelector(`[data-chart="${chart}"] .panel-note`) as HTMLElement).textContent = text;
  };
  note(
    'what-fails',
    'How many city pairs can still reach each other, as more of the network breaks. The shape on each line says what breaks; dashed: the satellites turn their lasers to new partners after the break.',
  );
  note('pattern', 'The same, when random satellites break, for satellites with 3, 4 or 6 lasers each.');
  note(
    'percolation',
    'The biggest group of working satellites still joined by lasers, as a share of all working ones, on a test group at one height. Solid: satellites break; dashed: lasers break. Each upright dotted line marks where theory says such a network falls apart.',
  );
  // The two share charts on one scale, from just below the lowest share either shows (display only).
  const lowest = Math.min(
    ...[...WHAT_FAILS, ...BY_PATTERN].flatMap((d) => pointsOf(recovery, d.match, 'share', 'reach_reference_mean').y.filter((v): v is number => v !== null)),
  );
  const shareRange: [number, number] = [Number.isFinite(lowest) ? Math.max(0, Math.floor(lowest * 20) / 20 - 0.05) : 0, 1.01];
  const shareChart = (chart: string, defs: LineDef[]): void => {
    const lines = defs.map((d) => ({ d, p: pointsOf(recovery, d.match, 'share', 'reach_reference_mean') }));
    const x = lines[0]?.p.x ?? [];
    plots.push(
      lineChart(root.querySelector(`[data-chart="${chart}"] .line-plot`) as HTMLElement, {
        x,
        lines: lines.map(({ d, p }) => toLine(d, x, p)),
        xLabel: 'Share broken',
        yLabel: 'City pairs still connected',
        height: 260,
        xLog: true,
        xTicks: (min, max) => SHARE_TICKS.filter((v) => v >= min * 0.999 && v <= max * 1.001),
        xValue: formatShare,
        yValue: formatShare,
        yRange: shareRange,
      }),
    );
  };
  shareChart('what-fails', WHAT_FAILS);
  shareChart('pattern', BY_PATTERN);
  const perc = PERCOLATION.map((d) => ({ d, p: pointsOf(percolation, d.match, 'failed_share', 'mean_largest_piece_share') }));
  const px = perc[0]?.p.x ?? [];
  plots.push(
    lineChart(root.querySelector('[data-chart="percolation"] .line-plot') as HTMLElement, {
      x: px,
      lines: perc.map(({ d, p }) => toLine(d, px, p)),
      xLabel: 'Share broken',
      yLabel: 'Biggest joined group',
      height: 260,
      xValue: formatShare,
      yValue: formatShare,
      yRange: [0, 1.02],
      marks: PERCOLATION.flatMap((d) => {
        const t = pointsOf(percolation, d.match, 'failed_share', 'threshold_failed_share').y[0];
        return typeof t === 'number' ? [{ x: t, color: d.color, shape: d.marker }] : [];
      }),
    }),
  );
  el('.numbers-tables').append(
    numbersTable(recovery, 'recovery_summary', ['group', 'scenario', 'pattern', 'share', 'repoint', 'runs', 'reach_reference_mean', 'reach_reference_p2_5', 'reach_reference_p97_5']),
    numbersTable(percolation, 'percolation', percolation.columns),
  );

  // The globe at one instant.
  const globeEl = el('.stage-globe');
  const stage = new Stage(globeEl);
  const three = links.get('three') as Links;
  const field = new SatelliteField(data, new Uint32Array(0), Math.max(...[...links.values()].map((l) => l.pairs.length)));
  const segments = field.segments(THEME.links, LINK_OPACITY);
  stage.add(segments);
  const colors = new Float32Array(field.count * 3);
  const sizes = new Float32Array(field.count);
  const shapes = new Float32Array(field.count);
  const satPoints = field.points(colors, sizes, stage.dotMaterial(), undefined, undefined, shapes);
  stage.add(satPoints);
  const sites = siteScenePositions(data);
  const placer = new PathPlacer(field, sites);
  const pairs = story.steps[0]?.routes ?? [];
  const cities = [...new Set(pairs.flatMap((p) => [p.from, p.to]))];
  stage.add(
    stage.fixedDots(
      cities.flatMap((i) => placer.site(i)),
      cities.map(() => MARKS.city.color),
      cities.map(() => 10),
      cities.map(() => MARKS.city.shape),
    ),
  );
  const baselines = pairs.map(() => stage.path(BASELINE_STYLE.color, 1.6, BASELINE_OPACITY));
  const routes = pairs.map(() => stage.path(ROUTE_STYLE.color, ROUTE_STYLE.width));
  stage.globe.lookFrom([0, 1, 2].map((k) => cities.reduce((sum, i) => sum + (sites[3 * i + k] as number), 0)), CAMERA_DISTANCE);
  writeLegend(el('.resilience-legend'));

  let step = story.steps[0] as Step;
  let fresh = true;
  const show = (index: number): void => {
    step = story.steps[index] as Step;
    const planned = links.get(step.pattern) ?? three;
    field.setLinks(pickLinks(planned.pairs, step.planned_links_not_failed));
    const failed = new Uint8Array(field.count);
    for (const s of step.failed_satellites) failed[s] = 1;
    colors.set(rgbArray(data.satellites.shell.map((s, i) => (failed[i] === 1 ? FAILED.color : shellColor(s)))));
    failed.forEach((f, i) => (sizes[i] = f === 1 ? FAILED_SIZE : WORKING_SIZE));
    shapes.set(shapeArray(Array.from(failed, (f) => (f === 1 ? FAILED.shape : 'disc'))));
    satPoints.geometry.getAttribute('aColor').needsUpdate = true;
    satPoints.geometry.getAttribute('aSize').needsUpdate = true;
    satPoints.geometry.getAttribute('aShape').needsUpdate = true;
    section.dataset.step = step.name;
    globeEl.dataset.failedSatellites = String(step.failed_satellites.length);
    globeEl.dataset.linksDrawn = String(step.planned_links_not_failed.length);
    el('.instant-text').textContent =
      `${formatCount(step.failed_satellites.length)} satellites broken (${stepTitle(step)}). Each bright line is a pair’s new path around the breaks.`;
    writePairs(el('.pair-list'), step, data);
    writeExample(el('.sources-body .example'), step);
    el('.network-note').textContent =
      'The working satellites fall into pieces: groups that lasers join to each other but not to the rest. ' +
      'Lasers never join two groups of different height and tilt, so all of them together are always several pieces.';
    el('.network-panel .table-scroll').replaceChildren(networkTable(step));
    fresh = true;
  };
  const menu = segmented(
    'Failed share',
    story.steps.map((s) => ({ value: s.name, text: stepTitle(s) })),
    show,
  );
  menu.select(0);
  el('.control-bar .control-group').append(menu.element);
  show(0);

  const stop = frameLoop(() => {
    if (fresh) {
      fresh = false;
      field.update(step.t_s);
      step.routes.forEach((p, i) => {
        const after = afterRecovery(p.reference);
        (routes[i] as ThickPath).set(after.routed ? placer.points(routeNodes(p.from, after.route, p.to)) : []);
        const before = afterRecovery(p.baseline);
        (baselines[i] as ThickPath).set(before.routed ? placer.points(routeNodes(p.from, before.route, p.to)) : []);
      });
    }
    stage.render();
    document.body.dataset.ready = 'true';
  });

  return () => {
    stop();
    for (const p of plots) p.dispose();
    stage.dispose();
    root.replaceChildren();
  };
}

function toLine(d: LineDef, x: readonly number[], p: Points): Line {
  return { label: d.label, color: d.color, marker: d.marker, values: alignTo(x, p), ...(d.dash === undefined ? {} : { dash: d.dash }) };
}

function stepTitle(s: Step): string {
  return s.scenario === 'attack' ? `the busiest ${formatShare(s.share)}` : `${formatShare(s.share)} at random`;
}

const CAUSES: Record<string, string> = { no_path: 'no chain of satellites joins the two cities', no_coverage: 'no satellite in view' };

/** One line per study pair; data attributes for the browser test (route as catalogue numbers). */
function writePairs(list: HTMLElement, step: Step, data: CommonData): void {
  list.replaceChildren(
    ...step.routes.map((p, i) => {
      const li = document.createElement('li');
      const after = afterRecovery(p.reference);
      li.dataset.pair = String(i);
      li.dataset.from = data.sites[p.from]?.name ?? '';
      li.dataset.to = data.sites[p.to]?.name ?? '';
      li.dataset.state = p.reference.state;
      li.dataset.routed = String(after.routed);
      li.dataset.route = after.route.map((s) => data.satellites.norad[s]).join(' ');
      li.dataset.oneWayMs = after.routed && p.reference.one_way_ms !== null ? String(p.reference.one_way_ms) : '';
      const name = document.createElement('span');
      name.className = 'pair-name';
      name.textContent = `${li.dataset.from} to ${li.dataset.to}`;
      const state = document.createElement('span');
      state.className = 'pair-state';
      const cause = p.reference.drop_cause === null ? '' : ` (${CAUSES[p.reference.drop_cause] ?? p.reference.drop_cause.replaceAll('_', ' ')})`;
      state.textContent = after.routed
        ? `${formatMs(p.reference.one_way_ms)} over ${formatCount(after.route.length)} ${after.route.length === 1 ? 'satellite' : 'satellites'} (${formatMs(p.baseline.one_way_ms)} with nothing broken)`
        : `no route${cause}`;
      li.append(name, state);
      return li;
    }),
  );
}

function writeLegend(list: HTMLElement): void {
  const items: [Mark, string][] = [
    [line(ROUTE_STYLE), 'New path around the breaks'],
    [line({ ...BASELINE_STYLE, width: 1.6 }, BASELINE_OPACITY), 'Path with nothing broken'],
    [dot(MARKS.city), 'City'],
    [dot(FAILED), 'Broken satellite'],
    [dot({ color: shellColor(0), shape: 'disc' }), 'Working satellite'],
    [line({ color: THEME.links, width: 1, dash: [] }, LINK_OPACITY), 'Laser link still working'],
  ];
  list.replaceChildren(
    ...items.map(([mark, text]) => {
      const li = document.createElement('li');
      li.textContent = text;
      setSwatch(li, mark);
      return li;
    }),
  );
}

function networkTable(step: Step): HTMLTableElement {
  const t = document.createElement('table');
  t.innerHTML = `<thead><tr><th scope="col">Which satellites</th><th scope="col">Working satellites</th><th scope="col">Pieces</th>
    <th scope="col">Biggest piece</th><th scope="col">Its share</th></tr></thead><tbody></tbody>`;
  const body = t.querySelector('tbody') as HTMLElement;
  for (const r of step.network) {
    const tr = document.createElement('tr');
    const cells: [string, string][] = [
      ['working_satellites', formatCount(r.working_satellites)],
      ['pieces', formatCount(r.pieces)],
      ['largest_piece', formatCount(r.largest_piece)],
      ['largest_piece_share', formatShare(r.largest_piece_share)],
    ];
    const th = document.createElement('th');
    th.scope = 'row';
    th.textContent = r.scope === 'all' ? 'All groups' : shellName(r.scope);
    tr.append(th);
    for (const [field, text] of cells) {
      const td = document.createElement('td');
      td.dataset.scope = r.scope;
      td.dataset.field = field;
      td.textContent = text;
      tr.append(td);
    }
    body.append(tr);
  }
  return t;
}

/** A committed file's rows as the export copied them, for reading and for the browser test. */
function numbersTable(table: Table, name: string, columns: readonly string[]): HTMLElement {
  const wrap = document.createElement('div');
  wrap.className = 'table-scroll';
  const caption = document.createElement('p');
  caption.className = 'panel-note';
  caption.textContent = table.source;
  const t = document.createElement('table');
  t.dataset.source = name;
  const head = document.createElement('tr');
  for (const c of columns) {
    const th = document.createElement('th');
    th.scope = 'col';
    th.textContent = c;
    head.append(th);
  }
  const thead = document.createElement('thead');
  thead.append(head);
  const body = document.createElement('tbody');
  table.rows.forEach((row, i) => {
    const tr = document.createElement('tr');
    tr.dataset.row = String(i);
    for (const c of columns) {
      const td = document.createElement('td');
      td.dataset.column = c;
      const v = cellOf(table, row, c);
      td.textContent = v === null ? '' : String(v);
      tr.append(td);
    }
    body.append(tr);
  });
  t.append(thead, body);
  wrap.append(caption, t);
  return wrap;
}

/** The fold's part about the example on the globe, which changes with the chosen share. */
function writeExample(p: HTMLElement, step: Step): void {
  p.textContent =
    `The example on the globe: ${stepTitle(step)} broken at ${step.utc.replace('T', ' ')} UTC (world time) ` +
    `(${step.seed === null ? 'the busiest satellites at that moment' : `random draw ${step.seed}`}), ` +
    `${formatCount(step.failed_satellites.length)} satellites. The faint lines are the ` +
    `${formatCount(step.planned_links_not_failed.length)} planned laser links (${patternShort(step.pattern)}) that did not break; the ` +
    `${formatCount(step.failed_links.length)} links of the broken satellites are left out. A planned link can also be out of ` +
    'sight at that moment, which no program records. The new paths are chosen by a planner that knows about every break.' +
    (step.sources.network === undefined ? '' : ` Pieces of the network: ${step.sources.network}.`);
}

function writeText(root: HTMLElement, story: ResilienceStory): void {
  const q = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const t = story.charts.recovery_summary;
  const at = (match: Record<string, Cell>): { share: number; reach: number | null } | null => {
    const p = pointsOf(t, match, 'share', 'reach_reference_mean');
    const last = p.x.length - 1;
    return last < 0 ? null : { share: p.x[last] as number, reach: p.y[last] ?? null };
  };
  const random = at(WHAT_FAILS[0]?.match ?? {});
  const attack = at(WHAT_FAILS[2]?.match ?? {});
  const six = at(BY_PATTERN[2]?.match ?? {});
  let result = '';
  if (random !== null && attack !== null) {
    result =
      `With ${formatShare(random.share)} of the satellites broken at random, ${formatShare(random.reach)} of the city pairs can still reach each other; ` +
      `when the busiest ${formatShare(attack.share)} break, ${formatShare(attack.reach)} can.`;
  }
  if (six !== null) result += ` With ${patternShort('six')} and ${formatShare(six.share)} broken at random, ${formatShare(six.reach)} can.`;
  q('.lede').textContent =
    'What if many satellites stop working at the same time? Here the network has had time to find new paths around ' +
    'every break. The globe shows one example; the charts below it show how much still connects as more breaks.';
  q('.try-text').textContent =
    'Pick how much breaks, above the globe, and see which paths survive. Then follow the charts from left (a little broken) to right (a lot).';
  fillFold(q('.how-body'), [
    'On the globe, crosses are broken satellites and small dots working ones. For each pair of cities, the bright line is its new path ' +
      'and the faint thin line the path it had with nothing broken. The list beside the globe gives each pair’s travel time: how long a ' +
      'message takes, one way, in thousandths of a second (ms).',
    'In the charts, the shape on each line tells the lines apart, as named under each chart. “Lasers re-aimed” means the satellites ' +
      'next to a broken one turn their lasers to new partners.',
    'Drag the globe to turn it; scroll or pinch to zoom. Point at a chart to read its values.',
  ]);
  const example = document.createElement('p');
  example.className = 'example';
  fillFold(q('.sources-body'), [
    result,
    'The charts are averages over many random draws, from runs of the simulator’s failures program (Phase 4): ' +
      `${t.source} and ${story.charts.percolation.source}. The test group in the last chart flies at one height.`,
    example,
  ]);
}

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
import { FAILURE_KIND_COLORS, patternColor, shellColor, THEME } from '../../theme';
import { frameLoop, segmented } from '../../ui/controls';
import type { Story, StoryContext } from '../story';
import { cellOf, parseResilienceStory, type Cell, type ResilienceStory, type Step, type Table } from './data';
import { afterRecovery, alignTo, pickLinks, pointsOf, type Points } from './state';

const PATTERNS = ['plus-grid', 'three', 'six'] as const;
const CAMERA_DISTANCE = 4.2;
const FAILED_COLOR = '#56606b';
const DASH = [7, 5];
const SHARE_TICKS = [0.001, 0.003, 0.01, 0.03, 0.1, 0.3];

export const resilienceStory: Story = {
  id: 'resilience',
  title: 'The network falling apart',
  mount,
};

interface LineDef {
  label: string;
  color: string;
  dash?: number[];
  match: Record<string, Cell>;
}

const WHAT_FAILS: LineDef[] = [
  { label: 'Random satellites', color: FAILURE_KIND_COLORS['random-satellites'] as string, match: { group: 'random-satellites', scenario: 'random-satellites', pattern: 'three', repoint: 'off' } },
  { label: 'Random lasers', color: FAILURE_KIND_COLORS['random-lasers'] as string, match: { group: 'random-lasers', scenario: 'random-lasers', pattern: 'three', repoint: 'off' } },
  { label: 'Busiest satellites first', color: FAILURE_KIND_COLORS.attack as string, match: { group: 'attack', scenario: 'attack', pattern: 'three', repoint: 'off' } },
  { label: 'Random satellites, re-pointed', color: FAILURE_KIND_COLORS['random-satellites'] as string, dash: DASH, match: { group: 'recovery-repointing', scenario: 'random-satellites', pattern: 'three', repoint: 'on' } },
  { label: 'Busiest first, re-pointed', color: FAILURE_KIND_COLORS.attack as string, dash: DASH, match: { group: 'recovery-repointing', scenario: 'attack', pattern: 'three', repoint: 'on' } },
];

const BY_PATTERN: LineDef[] = [
  { label: 'plus-grid', color: patternColor('plus-grid'), match: { group: 'pattern', scenario: 'random-satellites', pattern: 'plus-grid', repoint: 'off' } },
  { label: 'three', color: patternColor('three'), match: { group: 'random-satellites', scenario: 'random-satellites', pattern: 'three', repoint: 'off' } },
  { label: 'six', color: patternColor('six'), match: { group: 'pattern', scenario: 'random-satellites', pattern: 'six', repoint: 'off' } },
];

const PERCOLATION: LineDef[] = [
  { label: 'plus-grid, satellites fail', color: patternColor('plus-grid'), match: { links: 'plus-grid', kind: 'satellites' } },
  { label: 'plus-grid, lasers fail', color: patternColor('plus-grid'), dash: DASH, match: { links: 'plus-grid', kind: 'laser-links' } },
  { label: 'three, satellites fail', color: patternColor('three'), match: { links: 'three', kind: 'satellites' } },
  { label: 'three, lasers fail', color: patternColor('three'), dash: DASH, match: { links: 'three', kind: 'laser-links' } },
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
      <div class="intro">
        <h1>The network falling apart</h1>
        <div class="intro-text">
          <p class="lede"></p>
          <p class="how"></p>
        </div>
      </div>
      <div class="panels charts">
        <section class="panel" data-chart="what-fails">
          <h2>What fails</h2>
          <p class="panel-note"></p>
          <div class="line-plot"></div>
        </section>
        <section class="panel" data-chart="pattern">
          <h2>The laser pattern</h2>
          <p class="panel-note"></p>
          <div class="line-plot"></div>
        </section>
        <section class="panel" data-chart="percolation">
          <h2>When one piece becomes many</h2>
          <p class="panel-note"></p>
          <div class="line-plot"></div>
        </section>
        <details class="panel numbers">
          <summary>The numbers behind these charts</summary>
          <div class="numbers-tables"></div>
        </details>
      </div>
      <div class="instant">
        <div class="instant-head">
          <h2>One draw, on the real constellation</h2>
          <p class="instant-text"></p>
        </div>
        <div class="control-bar"></div>
        <div class="stage">
          <div class="stage-globe"></div>
          <aside class="stage-side">
            <h2>The study pairs after recovery</h2>
            <ul class="pair-list"></ul>
            <ul class="legend-list resilience-legend"></ul>
          </aside>
        </div>
        <div class="panels">
          <section class="panel network-panel">
            <h2>Pieces of the network</h2>
            <p class="panel-note network-note"></p>
            <div class="table-scroll"></div>
          </section>
        </div>
      </div>
    </section>`;
  const el = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const section = el('.story');
  writeText(el('.lede'), el('.how'), story);

  const recovery = story.charts.recovery_summary;
  const percolation = story.charts.percolation;
  const plots: ResponsivePlot[] = [];
  const note = (chart: string, text: string): void => {
    (root.querySelector(`[data-chart="${chart}"] .panel-note`) as HTMLElement).textContent = text;
  };
  note(
    'what-fails',
    `Share of the city pairs that still have a route once every failure is known, against the share of satellites or lasers failed (mean over the runs; ${recovery.source}). Pattern “three”; dashed: the satellites point their lasers again at new partners.`,
  );
  note('pattern', `The same share for random satellite failures under each laser pattern (${recovery.source}).`);
  note(
    'percolation',
    `The largest connected part as a share of the working satellites, on Phase 4’s test shell of one orbit height (${percolation.source}). Dotted lines: where theory says an endless grid of that kind falls apart.`,
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
        xLabel: 'Failed share',
        yLabel: 'Pairs with a route',
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
      xLabel: 'Failed share',
      yLabel: 'Largest part',
      height: 260,
      xValue: formatShare,
      yValue: formatShare,
      yRange: [0, 1.02],
      marks: PERCOLATION.flatMap((d) => {
        const t = pointsOf(percolation, d.match, 'failed_share', 'threshold_failed_share').y[0];
        return typeof t === 'number' ? [{ x: t, color: d.color }] : [];
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
  const segments = field.segments(THEME.links, 0.3);
  stage.add(segments);
  const colors = new Float32Array(field.count * 3);
  const sizes = new Float32Array(field.count);
  const satPoints = field.points(colors, sizes, stage.dotMaterial());
  stage.add(satPoints);
  const sites = siteScenePositions(data);
  const placer = new PathPlacer(field, sites);
  const pairs = story.steps[0]?.routes ?? [];
  const cities = [...new Set(pairs.flatMap((p) => [p.from, p.to]))];
  stage.add(stage.fixedDots(cities.flatMap((i) => placer.site(i)), cities.map(() => THEME.city), cities.map(() => 8)));
  const baselines = pairs.map(() => stage.path(THEME.baseline, 1.6, 0.35));
  const routes = pairs.map(() => stage.path(THEME.delivered, 3.6));
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
    colors.set(rgbArray(data.satellites.shell.map((s, i) => (failed[i] === 1 ? FAILED_COLOR : shellColor(s)))));
    failed.forEach((f, i) => (sizes[i] = f === 1 ? 2.6 : 1.8));
    satPoints.geometry.getAttribute('aColor').needsUpdate = true;
    satPoints.geometry.getAttribute('aSize').needsUpdate = true;
    section.dataset.step = step.name;
    globeEl.dataset.failedSatellites = String(step.failed_satellites.length);
    globeEl.dataset.linksDrawn = String(step.planned_links_not_failed.length);
    el('.instant-text').textContent =
      `${stepTitle(step)} failed at ${step.utc.replace('T', ' ')} UTC (${step.seed === null ? 'the busiest satellites at that instant' : `random draw ${step.seed}`}), ` +
      `${formatCount(step.failed_satellites.length)} satellites. Grey dots are the failed satellites. The faint lines are the ` +
      `${formatCount(step.planned_links_not_failed.length)} planned laser links of the pattern “${step.pattern}” that did not fail (the ` +
      `${formatCount(step.failed_links.length)} links of the failed satellites are left out). They are planned links, not the links that work: ` +
      'a planned link can also be out of sight at this instant, and no program writes that. Teal: each pair’s route after recovery, ' +
      'chosen by a planner that knows every failure; faint white: its route with nothing failed.';
    writePairs(el('.pair-list'), step, data);
    el('.network-note').textContent =
      `From the run’s network file (${step.sources.network ?? ''}): the working satellites split into pieces that lasers do not join. ` +
      'Lasers never join two shells, so all shells together are always several pieces.';
    el('.network-panel .table-scroll').replaceChildren(networkTable(step));
    fresh = true;
  };
  const menu = segmented(
    'Failed share',
    story.steps.map((s) => ({ value: s.name, text: stepTitle(s) })),
    show,
  );
  menu.select(0);
  el('.control-bar').append(menu.element);
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
  return { label: d.label, color: d.color, values: alignTo(x, p), ...(d.dash === undefined ? {} : { dash: d.dash }) };
}

function stepTitle(s: Step): string {
  return s.scenario === 'attack' ? `${formatShare(s.share)}, busiest first` : `${formatShare(s.share)} at random`;
}

const CAUSES: Record<string, string> = { no_path: 'no chain of lasers joins the cities', no_coverage: 'no satellite in view' };

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
        ? `${formatMs(p.reference.one_way_ms)} over ${formatCount(after.route.length)} satellites; with nothing failed ${formatMs(p.baseline.one_way_ms)}`
        : `no route${cause}`;
      li.append(name, state);
      return li;
    }),
  );
}

function writeLegend(list: HTMLElement): void {
  const items: [string, string][] = [
    [THEME.delivered, 'Route after recovery'],
    [THEME.baseline, 'Route with nothing failed'],
    [FAILED_COLOR, 'Failed satellite'],
    [THEME.links, 'Planned laser link that did not fail'],
  ];
  list.replaceChildren(
    ...items.map(([color, text]) => {
      const li = document.createElement('li');
      li.style.setProperty('--swatch', color);
      li.textContent = text;
      return li;
    }),
  );
}

function networkTable(step: Step): HTMLTableElement {
  const t = document.createElement('table');
  t.innerHTML = `<thead><tr><th scope="col">Scope</th><th scope="col">Working satellites</th><th scope="col">Pieces</th>
    <th scope="col">Largest piece</th><th scope="col">Its share</th></tr></thead><tbody></tbody>`;
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
    th.textContent = r.scope === 'all' ? 'All shells' : `Shell ${r.scope}`;
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

function writeText(lede: HTMLElement, how: HTMLElement, story: ResilienceStory): void {
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
      ` With ${formatShare(random.share)} of the satellites failed at random, ${formatShare(random.reach)} of the pairs still have a route; ` +
      `when the busiest ${formatShare(attack.share)} fail, ${formatShare(attack.reach)} do.`;
  }
  if (six !== null) result += ` With the pattern “six” and ${formatShare(six.share)} failed at random, ${formatShare(six.reach)} do.`;
  lede.textContent =
    'What happens when many satellites or laser links stop working at once? Here every failure is old news: everybody ' +
    'knows about it, and the route of each pair of cities is planned again around it. A pair still has a route if some chain ' +
    'of working satellites and lasers joins its two cities.' + result;
  how.textContent =
    'The charts are Phase 4’s summaries over many random draws. A connected part is a group of satellites that lasers join to each ' +
    'other; when too much fails, the network breaks into many small parts. Below, one draw on the real constellation: pick a failed share ' +
    'to see what is left. Drag to turn the globe.';
}

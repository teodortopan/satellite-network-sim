// Story: a connection over time (spec 4.6). Replays a Phase 3 `routing` run on the best-case
// model: for one direction of a study pair, the route each rule (strategy) holds at the
// clock's time, its breaks, its delay over the run and the engine's summary. Everything below
// the text is a RouteView; this file adds the pair menu, the direction buttons and the words.

import type { CommonData } from '../../data/common';
import { parseLinks } from '../../data/common';
import { formatCount, formatMs, formatSeconds, formatSpan } from '../../format';
import { dropdown, segmented } from '../../ui/controls';
import { fillFold, introMarkup, legendList } from '../../ui/intro';
import { patternWords } from '../../ui/words';
import type { Story, StoryContext } from '../story';
import { parseRouteStory, type Direction, type RouteStory } from './data';
import { RouteView, STRATEGY_NOTES, strategySwatch, strategyTitle, type Endpoint } from './view';

export { STRATEGY_TITLES } from './view';

export const routeStory: Story = {
  id: 'route',
  title: 'A connection over time',
  summary: 'Follow one connection between two cities for a few minutes as the satellites move.',
  mount,
};

/** The pairs of the story file: each unordered pair once, with its two directions. */
function pairsOf(story: RouteStory): { a: number; b: number; directions: Direction[] }[] {
  const pairs: { a: number; b: number; directions: Direction[] }[] = [];
  for (const d of story.directions) {
    const pair = pairs.find((p) => (p.a === d.from && p.b === d.to) || (p.a === d.to && p.b === d.from));
    if (pair === undefined) pairs.push({ a: d.from, b: d.to, directions: [d] });
    else pair.directions.push(d);
  }
  return pairs;
}

async function mount({ data, root }: StoryContext): Promise<() => void> {
  const { manifest } = data.source;
  const story = await data.source.json('story-route.json', (v, c) =>
    parseRouteStory(v, c, manifest.satellites, data.sites, manifest.settings.window_s),
  );
  const links = await data.source.json(`links-${story.settings.pattern}.json`, (v, c) => parseLinks(v, c, manifest.satellites));
  const name = (i: number): string => data.sites[i]?.name ?? '';
  const endpoint = (i: number): Endpoint => {
    const s = data.sites[i];
    return { name: s?.name ?? '', lat_deg: s?.lat_deg ?? 0, lon_deg: s?.lon_deg ?? 0 };
  };

  root.innerHTML = `
    <section class="story route">
      ${introMarkup('A connection over time')}
      <div class="view-host"></div>
    </section>`;
  const el = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const section = el('.story');
  writeText(root, story, data);

  const view = new RouteView(el('.view-host'), data, links);
  const pairs = pairsOf(story);
  let pairIndex = 0;
  let dirIndex = 0;
  const pairMenu = dropdown(
    'Cities',
    pairs.map((p, i) => ({ value: String(i), text: `${name(p.a)} and ${name(p.b)}` })),
    (i) => {
      pairIndex = i;
      dirIndex = 0;
      showDirection();
    },
  );
  const dirHost = document.createElement('div');
  view.controls.append(pairMenu, dirHost);

  function showDirection(): void {
    const pair = pairs[pairIndex] as (typeof pairs)[number];
    const direction = pair.directions[dirIndex] as Direction;
    const menu = segmented(
      'Direction',
      pair.directions.map((d, i) => ({ value: String(i), text: `${name(d.from)} to ${name(d.to)}` })),
      (i) => {
        dirIndex = i;
        showDirection();
      },
    );
    menu.select(dirIndex);
    dirHost.replaceChildren(menu.element);
    section.dataset.from = name(direction.from);
    section.dataset.to = name(direction.to);
    view.show({
      settings: story.settings,
      run_start_s: story.run_start_s,
      direction,
      from: endpoint(direction.from),
      to: endpoint(direction.to),
    });
  }
  showDirection();

  return () => {
    view.dispose();
    root.replaceChildren();
  };
}

/** The story's words: what is shown, how to read it, and the main result for the first pair. */
function writeText(root: HTMLElement, story: RouteStory, data: CommonData): void {
  const q = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const name = (i: number): string => data.sites[i]?.name ?? '';
  const first = story.directions[0] as Direction;
  const strategies = story.settings.strategies;
  q('.lede').textContent =
    `A message from ${name(first.from)} to ${name(first.to)} goes up to a satellite, jumps from satellite to satellite ` +
    `along laser links, and comes back down. The satellites move fast, so the path keeps changing: here are ` +
    `${formatSpan(story.settings.duration_s)} of it under ${formatCount(strategies.length)} different rules for choosing the path.`;
  q('.try-text').textContent =
    'Press Play and watch the paths move. Switch rules on and off with the buttons above the globe. Pick another pair of cities.';

  fillFold(q('.how-body'), [
    'Each rule has its own line, the same on the globe, beside it, in the charts and in the table:',
    legendList(
      strategies.map((s) => [strategySwatch(s), `${strategyTitle(s)}: ${STRATEGY_NOTES[s] ?? ''}`.replace(/: $/, '')] as const),
      'legend-list fold-legend',
    ),
    'Travel time is how long a message takes to get there, one way, in thousandths of a second (ms). ' +
      'When a rule has no connection, its last path is drawn dotted and dimmer, and a cross marks what broke.',
    'Drag the globe to turn it; scroll or pinch to zoom.',
  ]);

  const results = strategies.map((s) => {
    const sum = first.strategies[s]?.summary;
    if (sum === undefined) return '';
    return `${strategyTitle(s)} switched route ${formatCount(sum.changes)} ${sum.changes === 1 ? 'time' : 'times'}, with a typical travel time of ${formatMs(sum.delay_median_ms)}`;
  });
  fillFold(q('.sources-body'), [
    `From ${name(first.from)} to ${name(first.to)}: ${results.filter((r) => r !== '').join('; ')}.`,
    `This is the best case: a city can use any satellite it can see. Here ${patternWords(story.settings.pattern)}. Routes are checked every ${formatSeconds(story.settings.replan_s)}. ` +
      `The run starts at ${story.settings.start_utc.replace('T', ' ')} UTC (world time).`,
    `Computed by the simulator’s routing program (Phase 3). Its files: ${story.sources.routes}, ${story.sources.drops} and ${story.sources.summary}.`,
  ]);
}

// Story: one failure, three reactions (spec 4.5). Replays Phase 4 `failures` runs: the three
// reaction modes side by side on one clock, the news as it spreads, and the run's measures.

import { Clock } from '../../clock/clock';
import { ClockView } from '../../clock/clock-view';
import { parseLinks, type CommonData } from '../../data/common';
import { GlobeView } from '../../globe/globe-view';
import { earth } from '../../globe/layers';
import { siteScenePositions } from '../../globe/paths';
import { SatelliteField } from '../../globe/satellite-field';
import { formatInstant } from '../../format';
import { BASELINE_STYLE, dot, LANE_STATE_STYLES, line, MARKS, THEME, type LineStyle } from '../../theme';
import { frameLoop, segmented } from '../../ui/controls';
import { fillFold, introMarkup, legendList } from '../../ui/intro';
import { patternWords } from '../../ui/words';
import type { Story, StoryContext } from '../story';
import { MODES, parseFailureStory, type FailureStory, type Scenario } from './data';
import { Lane, MODE_NOTES, MODE_TITLES } from './lane';
import { ContextChart, FactsPanel, measuresPanel } from './panels';
import { intervalAt } from './state';

/** The clock holds this long before t_f, so the network is still seen whole while it waits. */
const HOLD_LEAD_S = 0.05;
/** Where the clock starts, before the failure, and how fast it plays at first. */
const START_BEFORE_S = 10;
const START_SPEED = 5;
const CAMERA_DISTANCE = 3.3;

const SCENARIO_TITLES: Record<string, { title: string; text: string }> = {
  'route-satellite': {
    title: 'A satellite on the path',
    text: 'The satellite carrying the messages at that moment stops working.',
  },
  region: {
    title: 'A whole region',
    text: 'Every satellite over a region around the path stops working at once.',
  },
  gateway: {
    title: 'The ground station',
    text: 'The ground station the messages come down to stops working.',
  },
};

export const failureStory: Story = {
  id: 'failure',
  title: 'When a satellite breaks',
  summary: 'One piece of the network breaks; watch three ways it can notice and work around it.',
  mount,
};

async function mount({ data, root }: StoryContext): Promise<() => void> {
  const { manifest } = data.source;
  const story = await data.source.json('story-failure.json', (v, c) =>
    parseFailureStory(v, c, manifest.satellites, data.sites, manifest.settings.window_s),
  );
  const pattern = typeof story.settings.pattern === 'string' ? story.settings.pattern : 'three';
  const links = await data.source.json(`links-${pattern}.json`, (v, c) => parseLinks(v, c, manifest.satellites));

  root.innerHTML = `
    <section class="story failure">
      ${introMarkup('When a satellite breaks')}
      <div class="scenario-bar">
        <span class="control-label">What breaks</span>
        <p class="scenario-text"></p>
      </div>
      <div class="legend-strip"></div>
      <div class="clock-dock clock-above"></div>
      <div class="lanes"></div>
      <div class="panels"></div>
    </section>`;
  const el = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const marks = {
    delivered: line(LANE_STATE_STYLES.delivered as LineStyle),
    black_hole: line(LANE_STATE_STYLES.black_hole as LineStyle),
    loop: line(LANE_STATE_STYLES.loop as LineStyle),
    baseline: line(BASELINE_STYLE, 0.35),
    news: dot(MARKS.news),
    failed: dot(MARKS.failed),
    lost: dot(MARKS.blackHole),
    city: dot(MARKS.city),
    gateway: dot(MARKS.gateway),
  };
  el('.legend-strip').append(
    legendList([
      [marks.delivered, 'Path of the messages'],
      [marks.black_hole, 'Lost (dashed)'],
      [marks.loop, 'Going in circles (dotted)'],
      [marks.baseline, 'Path if nothing broke'],
      [marks.failed, 'Broken'],
      [marks.news, 'Knows about it'],
      [marks.city, 'City'],
      [marks.gateway, 'Ground station'],
    ]),
  );
  const from = data.sites[story.from]?.name ?? '';
  const to = data.sites[story.to]?.name ?? '';
  el('.lede').textContent =
    `Messages from ${from} to ${to} go up to a satellite, jump between satellites along laser links, and come ` +
    'down at a ground station. Then one piece on the way breaks. The three globes replay the same moment, each ' +
    'with a different way for the network to notice and work around it.';
  el('.try-text').textContent =
    'Press Play and watch the break: the clock slows to a crawl around it, because the first reactions are ' +
    'thousandths of a second apart. Compare the three globes. Drag one to turn it.';
  fillFold(el('.how-body'), [
    legendList(
      [
        [marks.delivered, 'Solid: messages arrive. Above each globe: “Arriving” and how long a message takes, one way, in thousandths of a second (ms).'],
        [marks.black_hole, 'Dashed: messages are sent into the broken piece and vanish there, a “black hole”. A hollow square marks the spot.'],
        [marks.loop, 'Dotted: messages go round in circles between satellites (a “loop”) and never arrive.'],
        [marks.baseline, 'Faint and thin: the path messages would take if nothing had broken.'],
        [marks.failed, 'A cross: what broke.'],
        [marks.news, 'A ring: a satellite that knows about the break.'],
      ],
      'legend-list fold-legend',
    ),
    'The three ways to react, one per globe:',
    ...MODES.map((m) => `${MODE_TITLES[m]}. ${MODE_NOTES[m]}`),
    'Under each globe, a list lights up as each step happens. Times are seconds from the start of the run.',
  ]);

  const lanesEl = el('.lanes');
  const globe = new GlobeView(lanesEl, THEME.night);
  const field = new SatelliteField(data, links.pairs);
  const sites = siteScenePositions(data);
  const earthGroup = earth({ earth: THEME.earth, land: THEME.land });

  let current: ScenarioView | null = null;
  const show = (index: number): void => {
    current?.dispose();
    const scenario = story.scenarios[index] as Scenario;
    menu.select(index);
    el('.scenario-text').textContent = SCENARIO_TITLES[scenario.scenario]?.text ?? '';
    writeSources(el('.sources-body'), story, scenario, pattern);
    root.querySelector('.story')?.setAttribute('data-scenario', scenario.scenario);
    current = new ScenarioView(scenario, story, data, field, sites, earthGroup, globe, {
      lanes: lanesEl,
      clock: el('.clock-dock'),
      panels: el('.panels'),
    });
  };
  const menu = segmented(
    'Scenario',
    story.scenarios.map((s) => ({ value: s.scenario, text: SCENARIO_TITLES[s.scenario]?.title ?? s.scenario })),
    show,
  );
  el('.scenario-text').before(menu.element);
  show(0);

  const stop = frameLoop((real_s) => current?.frame(real_s));

  return () => {
    stop();
    current?.dispose();
    globe.dispose();
    root.replaceChildren();
  };
}

/** Everything that belongs to one scenario: its clock, lanes and panels. */
class ScenarioView {
  private readonly clock: Clock;
  private readonly clockView: ClockView;
  private readonly lanes: Lane[];
  private readonly facts: FactsPanel;
  private readonly chart: ContextChart;
  private drawn_t = Number.NaN;

  constructor(
    private readonly scenario: Scenario,
    story: FailureStory,
    data: CommonData,
    private readonly field: SatelliteField,
    sites: Float32Array,
    earthGroup: ReturnType<typeof earth>,
    private readonly globe: GlobeView,
    places: { lanes: HTMLElement; clock: HTMLElement; panels: HTMLElement },
  ) {
    const ends = { from: story.from, to: story.to };
    this.lanes = MODES.map((mode) => new Lane(mode, scenario, data, field, sites, earthGroup, ends));
    places.lanes.append(...this.lanes.map((l) => l.element));
    globe.setViews(this.lanes.map((l) => l.view));

    // Look at the start of the route as it is when the failure comes.
    const base = intervalAt(scenario.lanes.baseline, scenario.failure_s);
    const aim = base?.gateway ?? story.to;
    globe.lookFrom(
      [0, 1, 2].map((k) => (sites[3 * story.from + k] as number) + (sites[3 * aim + k] as number)),
      CAMERA_DISTANCE,
    );

    this.clock = new Clock({
      duration_s: scenario.duration_s,
      spans: scenario.slow_motion,
      hold: { at_s: Math.max(scenario.failure_s - HOLD_LEAD_S, 0), real_s: scenario.pause_before_failure_real_s },
      speed: START_SPEED,
    });
    this.clock.seek(Math.max(scenario.failure_s - START_BEFORE_S, 0));
    this.clockView = new ClockView(this.clock, {
      marks: [{ t_s: scenario.failure_s, label: 'the break' }],
      spans: scenario.slow_motion,
      holdLabel: 'paused just before the break',
    });
    places.clock.append(this.clockView.element);

    this.facts = new FactsPanel(scenario, data);
    this.chart = new ContextChart(scenario);
    places.panels.append(this.facts.element, measuresPanel(scenario), this.chart.element);
  }

  frame(real_s: number): void {
    this.clock.tick(real_s);
    const t = this.clock.t;
    if (t !== this.drawn_t) {
      this.drawn_t = t;
      this.field.update(this.scenario.run_start_s + t);
      for (const lane of this.lanes) lane.update(t);
      this.facts.update(t);
    }
    for (const lane of this.lanes) lane.zoom = this.globe.zoom;
    this.clockView.update();
    this.globe.render();
    document.body.dataset.ready = 'true';
  }

  dispose(): void {
    this.clockView.dispose();
    this.chart.dispose();
    for (const lane of this.lanes) lane.dispose();
    this.facts.element.parentElement?.replaceChildren();
  }
}

/** The fold with the run's settings and files: what the main text leaves out. */
function writeSources(body: HTMLElement, story: FailureStory, scenario: Scenario, pattern: string): void {
  fillFold(body, [
    `The break happens at ${story.instant_utc.replace('T', ' ')} UTC (world time), ${formatInstant(scenario.failure_s)} into the run. ` +
      `In this run ${patternWords(pattern)}.`,
    'In the simulator these three ways are called planner, local repair and flooding. A planner on the ground ' +
      'plans every route; satellites tell it what they see, and their reports take time to arrive.',
    `Computed by the simulator’s failures program (Phase 4). Its files: ${scenario.sources.events} (what happened ` +
      `when) and ${scenario.sources.measures} (the numbers below); the comparison with other runs comes from ` +
      `${scenario.context.source}.`,
  ]);
}

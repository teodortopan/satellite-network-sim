// Story: one failure, three reactions (spec 4.5). Replays Phase 4 `failures` runs: the three
// reaction modes side by side on one clock, the news as it spreads, and the run's measures.

import { Clock } from '../../clock/clock';
import { ClockView } from '../../clock/clock-view';
import { parseLinks, type CommonData } from '../../data/common';
import { GlobeView } from '../../globe/globe-view';
import { earth } from '../../globe/layers';
import { SatelliteField } from '../../globe/satellite-field';
import { THEME } from '../../theme';
import type { Story, StoryContext } from '../story';
import { MODES, parseFailureStory, type FailureStory, type Scenario } from './data';
import { Lane, siteScenePositions } from './lane';
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
    title: 'A satellite on the route',
    text: 'The satellite carrying the route at that moment stops working.',
  },
  region: {
    title: 'A whole region',
    text: 'Every satellite over a region around the route stops working at once.',
  },
  gateway: {
    title: 'The route’s gateway',
    text: 'The gateway the route comes down to stops working.',
  },
};

export const failureStory: Story = {
  id: 'failure',
  title: 'One failure, three reactions',
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
      <div class="intro">
        <h1>One failure, three reactions</h1>
        <div class="intro-text">
          <p class="lede"></p>
          <p class="how">The bright line is the route at the clock’s time: <b class="k-delivered">teal</b>
            when traffic arrives, <b class="k-black-hole">amber</b> up to where it is lost in a black
            hole, <b class="k-loop">violet</b> while it goes round in a loop. The faint line is the
            route with no failure. Satellites turn <b class="k-news">yellow</b> when they have the news:
            the detectors in the first two lanes, every satellite the flood reaches in the third.
            Failed elements turn <b class="k-failed">red</b>. Drag to turn the globes, pinch or scroll
            to zoom.</p>
        </div>
      </div>
      <div class="scenario-bar">
        <div class="scenario-menu" role="radiogroup" aria-label="Scenario"></div>
        <p class="scenario-text"></p>
      </div>
      <div class="lanes"></div>
      <div class="clock-dock"></div>
      <div class="panels"></div>
    </section>`;
  const el = (selector: string): HTMLElement => root.querySelector(selector) as HTMLElement;
  const from = data.sites[story.from]?.name ?? '';
  const to = data.sites[story.to]?.name ?? '';
  el('.lede').textContent =
    `Traffic from ${from} to ${to} goes up to a satellite, across laser links between ` +
    `satellites, down to a gateway and on by fibre. At ${story.instant_utc.replace('T', ' ')} UTC ` +
    'something on that path fails. Each lane replays the same run under one way of reacting: how ' +
    'long it takes to notice, to find a way round, and to settle on the route a planner that ' +
    'knows everything would choose. The clock slows down around the failure, because the first ' +
    'reactions are milliseconds apart.';

  const lanesEl = el('.lanes');
  const globe = new GlobeView(lanesEl, THEME.night);
  const field = new SatelliteField(data, links.pairs);
  const sites = siteScenePositions(data);
  const earthGroup = earth({ earth: THEME.earth, land: THEME.land });

  let current: ScenarioView | null = null;
  const menu = el('.scenario-menu');
  const show = (index: number): void => {
    current?.dispose();
    const scenario = story.scenarios[index] as Scenario;
    menu.querySelectorAll('button').forEach((b, i) => b.setAttribute('aria-checked', String(i === index)));
    el('.scenario-text').textContent = SCENARIO_TITLES[scenario.scenario]?.text ?? '';
    root.querySelector('.story')?.setAttribute('data-scenario', scenario.scenario);
    current = new ScenarioView(scenario, story, data, field, sites, earthGroup, globe, {
      lanes: lanesEl,
      clock: el('.clock-dock'),
      panels: el('.panels'),
    });
  };
  story.scenarios.forEach((scenario, i) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.setAttribute('role', 'radio');
    button.dataset.scenario = scenario.scenario;
    button.textContent = SCENARIO_TITLES[scenario.scenario]?.title ?? scenario.scenario;
    button.addEventListener('click', () => show(i));
    menu.append(button);
  });
  show(0);

  let frame = 0;
  let last = performance.now();
  const loop = (now: number): void => {
    const real_s = Math.min((now - last) / 1000, 0.25);
    last = now;
    current?.frame(real_s);
    frame = requestAnimationFrame(loop);
  };
  frame = requestAnimationFrame(loop);

  return () => {
    cancelAnimationFrame(frame);
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
      marks: [{ t_s: scenario.failure_s, label: 'failure' }],
      spans: scenario.slow_motion,
      holdLabel: 'paused just before the failure',
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

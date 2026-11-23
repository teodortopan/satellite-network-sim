import { describe, expect, it } from 'vitest';
import { Check } from '../../data/check';
import { parseResilienceStory } from './data';
import { SITES, storyJson, THREE } from './fixture';
import { afterRecovery, alignTo, pickLinks, pointsOf } from './state';

const story = parseResilienceStory(storyJson(), new Check('story-resilience.json'), 4, SITES, 420, () => 3);
const step = story.steps[0];
const [routed, unrouted] = step?.routes ?? [];
if (step === undefined || routed === undefined || unrouted === undefined) throw new Error('the fixture has no step with two pairs');

describe('the resilience story lookups', () => {
  it('takes a chart line from the summary rows that match, in file order', () => {
    const t = story.charts.recovery_summary;
    const random = pointsOf(t, { group: 'random-satellites', pattern: 'three', repoint: 'off' }, 'share', 'reach_reference_mean');
    expect(random).toEqual({ x: [0.01, 0.3], y: [1.0, 0.944061] });
    expect(pointsOf(t, { group: 'pattern', pattern: 'six' }, 'share', 'reach_reference_mean')).toEqual({ x: [0.3], y: [null] });
    expect(pointsOf(t, { group: 'none' }, 'share', 'reach_reference_mean')).toEqual({ x: [], y: [] });
  });

  it('puts each line on the chart x, with nothing where it has no point', () => {
    expect(alignTo([0.01, 0.3], { x: [0.3], y: [0.76] })).toEqual([null, 0.76]);
  });

  it('draws exactly the planned links that did not fail', () => {
    expect(Array.from(pickLinks(THREE, step.planned_links_not_failed))).toEqual([2, 3]);
    expect(Array.from(pickLinks(THREE, [0, 2]))).toEqual([0, 1, 2, 3]);
  });

  it('shows the route after recovery, or no route', () => {
    expect(afterRecovery(routed.reference)).toEqual({ routed: true, route: [0, 2] });
    expect(afterRecovery(unrouted.reference)).toEqual({ routed: false, route: [] });
  });
});

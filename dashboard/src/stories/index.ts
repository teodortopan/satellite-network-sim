// The stories the page offers, in menu order. The first is the landing page (no address).

import { constellationStory } from './constellation';
import { exploreStory } from './explore';
import { failureStory } from './failure';
import { resilienceStory } from './resilience';
import { routeStory } from './route';
import type { Story } from './story';

export const STORIES: readonly Story[] = [exploreStory, failureStory, routeStory, constellationStory, resilienceStory];

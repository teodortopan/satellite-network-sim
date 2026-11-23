// The stories the page offers, in menu order.

import { constellationStory } from './constellation';
import { failureStory } from './failure';
import { resilienceStory } from './resilience';
import { routeStory } from './route';
import type { Story } from './story';

export const STORIES: readonly Story[] = [failureStory, routeStory, constellationStory, resilienceStory];

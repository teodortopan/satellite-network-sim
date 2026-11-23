// The stories the page offers, in menu order. The route, constellation and resilience stories
// (spec 4.6 to 4.8) are added here as each is built.

import { failureStory } from './failure';
import type { Story } from './story';

export const STORIES: readonly Story[] = [failureStory];

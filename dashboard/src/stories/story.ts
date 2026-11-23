// The interface every story implements (spec 4.5 to 4.8). A story is registered in
// stories/index.ts; the page lists the registered stories in its menu, mounts the chosen one
// into an empty element, and calls the returned function before mounting another.

import type { CommonData } from '../data/common';

export interface StoryContext {
  /** The files every story shares, already loaded and checked. */
  data: CommonData;
  /** An empty element the story owns until it is unmounted. */
  root: HTMLElement;
}

export interface Story {
  /** The story's place in the address (#failure). */
  id: string;
  /** The menu label. */
  title: string;
  /** Load the story's own files and draw it; resolves to the function that removes it. */
  mount(context: StoryContext): Promise<() => void>;
}

// The page: load the manifest and the shared files, list the stories, mount the chosen one.
// A file that fails its checks is reported in plain words and nothing is drawn from it.

import './styles.css';
import { DataError } from './data/check';
import { loadCommon, type CommonData } from './data/common';
import { openData } from './data/loader';
import { STORIES } from './stories';
import type { Story } from './stories/story';
import { applyTheme } from './theme';
import { formatCount, formatSpan } from './format';

const DATA_URL = 'data/';

applyTheme(document.documentElement);
const menu = document.querySelector('#story-menu') as HTMLElement;
const main = document.querySelector('#story') as HTMLElement;
const status = document.querySelector('#status') as HTMLElement;
const about = document.querySelector('#about-data') as HTMLElement;

function report(error: unknown): void {
  status.hidden = false;
  status.className = 'status error';
  status.setAttribute('role', 'alert');
  const heading = document.createElement('h2');
  const detail = document.createElement('p');
  if (error instanceof DataError) {
    heading.textContent = `The file ${error.file} could not be used, so nothing is drawn from it.`;
    detail.textContent = `${error.file}: ${error.problem}.`;
  } else {
    heading.textContent = 'The page stopped with an error.';
    detail.textContent = String(error);
  }
  status.replaceChildren(heading, detail);
  document.body.dataset.error = 'true';
  console.error(error);
}

function storyFromHash(): Story {
  // The story's id, before any query part a story keeps its own state in (#explore?from=...).
  const id = location.hash.replace(/^#/, '').split('?')[0] ?? '';
  return STORIES.find((s) => s.id === id) ?? (STORIES[0] as Story);
}

async function start(): Promise<void> {
  status.textContent = 'Loading the saved runs…';
  let data: CommonData;
  try {
    const source = await openData(DATA_URL, (url) => fetch(url));
    data = await loadCommon(source);
  } catch (error) {
    report(error);
    return;
  }
  status.hidden = true;
  const s = data.source.manifest.settings;
  about.textContent =
    `The satellites are real: ${formatCount(data.satellites.norad.length)} Starlink satellites at the positions ` +
    `published for them on ${data.source.manifest.snapshot}, followed for ${formatSpan(s.window_s)} from ` +
    `${s.window_start_utc.replace('T', ' ')} UTC (world time), one position every ${s.sample_step_s} s.`;

  buildMenu(menu, STORIES);

  let unmount: (() => void) | null = null;
  let mounting = Promise.resolve();
  const show = (): void => {
    const story = storyFromHash();
    mounting = mounting.then(async () => {
      unmount?.();
      unmount = null;
      menu.querySelectorAll<HTMLAnchorElement>('a[data-story]').forEach((a) => {
        if (a.dataset.story === story.id) a.setAttribute('aria-current', 'page');
        else a.removeAttribute('aria-current');
      });
      document.title = `${story.title} | Satellite network simulator`;
      try {
        unmount = await story.mount({ data, root: main });
      } catch (error) {
        main.replaceChildren();
        report(error);
      }
    });
  };
  window.addEventListener('hashchange', show);
  show();
}

/**
 * The menu: stories of the 'try' group first, as the main entries, then the saved runs under
 * their label. Each entry is the title with the story's one-line summary under it (shown on
 * wide screens; always its tooltip).
 */
function buildMenu(nav: HTMLElement, stories: readonly Story[]): void {
  const entry = (story: Story): HTMLAnchorElement => {
    const link = document.createElement('a');
    link.href = `#${story.id}`;
    link.dataset.story = story.id;
    link.title = story.summary;
    const title = document.createElement('span');
    title.className = 'menu-title';
    title.textContent = story.title;
    const summary = document.createElement('span');
    summary.className = 'menu-summary';
    summary.textContent = story.summary;
    link.append(title, summary);
    return link;
  };
  const tryIt = stories.filter((s) => s.group === 'try');
  const saved = stories.filter((s) => s.group !== 'try');
  if (tryIt.length > 0) {
    const group = document.createElement('div');
    group.className = 'menu-group menu-primary';
    group.append(...tryIt.map(entry));
    nav.append(group);
  }
  const group = document.createElement('div');
  group.className = 'menu-group menu-saved';
  const label = document.createElement('span');
  label.className = 'menu-label';
  label.textContent = 'Saved runs';
  group.append(label, ...saved.map(entry));
  nav.append(group);
}

void start();

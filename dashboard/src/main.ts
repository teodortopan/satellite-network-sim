// The page: load the manifest and the shared files, list the stories, mount the chosen one.
// A file that fails its checks is reported in plain words and nothing is drawn from it.

import './styles.css';
import { DataError } from './data/check';
import { loadCommon, type CommonData } from './data/common';
import { openData } from './data/loader';
import { STORIES } from './stories';
import type { Story } from './stories/story';
import { applyTheme } from './theme';

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
  const id = location.hash.replace(/^#/, '');
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
    `Satellites of the ${data.source.manifest.snapshot} snapshot, ${data.satellites.norad.length.toLocaleString('en-US')} ` +
    `in planes; positions every ${s.sample_step_s} s from ${s.window_start_utc.replace('T', ' ')} UTC ` +
    `for ${s.window_s} s (${s.mode} export).`;

  for (const story of STORIES) {
    const link = document.createElement('a');
    link.href = `#${story.id}`;
    link.textContent = story.title;
    link.dataset.story = story.id;
    menu.append(link);
  }

  let unmount: (() => void) | null = null;
  let mounting = Promise.resolve();
  const show = (): void => {
    const story = storyFromHash();
    mounting = mounting.then(async () => {
      unmount?.();
      unmount = null;
      menu.querySelectorAll('a').forEach((a) => {
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

void start();

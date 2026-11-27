// The table per strategy: the routing summary's numbers for the chosen direction, as the
// story file gives them, rounded only for display.

import { formatCount, formatMs, formatSeconds } from '../../format';
import { line, strategyStyle } from '../../theme';
import { setSwatch } from '../../ui/marks';
import type { Direction, RouteSummary } from './data';

const ROWS: { key: keyof RouteSummary; label: string; show: (s: RouteSummary) => string }[] = [
  { key: 'delay_median_ms', label: 'Typical travel time (median)', show: (s) => formatMs(s.delay_median_ms) },
  { key: 'changes', label: 'Times the route switched', show: (s) => formatCount(s.changes) },
  { key: 'drop_events', label: 'Times the connection broke', show: (s) => formatCount(s.drop_events) },
  { key: 'no_route_total_s', label: 'Time with no connection', show: (s) => formatSeconds(s.no_route_total_s) },
];

export function summaryTable(strategies: readonly string[], names: Record<string, string>, direction: Direction): HTMLTableElement {
  const table = document.createElement('table');
  table.className = 'summary-table';
  const head = document.createElement('tr');
  head.innerHTML = '<th scope="col"><span class="visually-hidden">Measure</span></th>';
  for (const s of strategies) {
    const th = document.createElement('th');
    th.scope = 'col';
    th.dataset.strategy = s;
    th.textContent = names[s] ?? s;
    const style = strategyStyle(s);
    setSwatch(th, line({ ...style, width: style.globeWidth * 0.6 }));
    head.append(th);
  }
  const thead = document.createElement('thead');
  thead.append(head);
  const body = document.createElement('tbody');
  for (const { key, label, show } of ROWS) {
    const tr = document.createElement('tr');
    const th = document.createElement('th');
    th.scope = 'row';
    th.textContent = label;
    tr.append(th);
    for (const s of strategies) {
      const summary = direction.strategies[s]?.summary;
      const td = document.createElement('td');
      td.dataset.strategy = s;
      td.dataset.measure = key;
      td.textContent = summary === undefined ? 'none' : show(summary);
      tr.append(td);
    }
    body.append(tr);
  }
  table.append(thead, body);
  return table;
}

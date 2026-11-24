// Convertible web app - the searchable target-format picker (a modal dialog
// with a combobox + listbox, grouped by category).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { categoryKey, organizeTargets, searchEntries } from '../model/targets.js';
import { formatLabel } from '../model/format.js';
import { clear, h, icon, uniqueId } from './dom.js';
import { createIncrementalRenderer } from './incremental.js';

/** Category jump links appear when the list has at least this many groups. */
const JUMP_MIN_GROUPS = 4;

let dialog = null;

function ensureDialog() {
  if (dialog) return dialog;
  dialog = h('dialog', { class: 'sheet picker', 'aria-labelledby': 'picker-title', 'data-testid': 'picker-dialog' });
  document.body.append(dialog);
  return dialog;
}

function optionRow(target, { current, idPrefix }) {
  const selected = target.id === current;
  return h(
    'li',
    {
      role: 'option',
      id: `${idPrefix}-${target.id}`,
      class: 'picker-option',
      'aria-selected': selected ? 'true' : 'false',
      'data-value': target.id,
      'data-testid': `target-option-${target.id}`,
    },
    h('span', { class: 'badge badge-square', 'data-cat': categoryKey(target.category) }, h('span', { class: 'badge-dot', 'aria-hidden': 'true' }), formatLabel(target.id)),
    h('span', { class: 'picker-option-name' }, target.compress ? `${target.name} (compress)` : target.name),
    target.lossy ? h('span', { class: 'picker-tag', title: 'Some information is discarded' }, 'lossy') : null,
    selected ? icon('check', { size: 18 }) : null,
  );
}

/**
 * Opens the picker. Resolves with the chosen target id, or null when dismissed.
 * @param {object} o
 * @param {string} o.title
 * @param {Array} o.targets          from cvt_targets_json
 * @param {string|null} o.current
 * @param {string|null} o.sourceCategory
 * @param {Map} o.formats            catalogue index (popularity)
 * @param {boolean} [o.suggest]      offer a "Suggested" group for long lists
 */
export function openPicker({ title, subtitle = '', targets, current = null, sourceCategory = null, formats = new Map(), suggest = true }) {
  const dlg = ensureDialog();
  clear(dlg);
  const idPrefix = uniqueId('opt');
  const listId = uniqueId('picker-list');
  const search = h('input', {
    type: 'search',
    class: 'picker-search',
    placeholder: 'Search formats',
    autocomplete: 'off',
    spellcheck: 'false',
    role: 'combobox',
    'aria-expanded': 'true',
    'aria-controls': listId,
    'aria-autocomplete': 'list',
    'aria-label': 'Search formats',
    'data-testid': 'picker-search',
  });
  const list = h('div', { class: 'picker-list', id: listId, role: 'listbox', 'aria-label': title });
  const empty = h('p', { class: 'picker-empty', hidden: true }, 'No matching formats.');
  const closeBtn = h('button', { type: 'button', class: 'icon-btn', 'aria-label': 'Close', 'data-testid': 'picker-close' }, icon('close'));
  const jumps = h('nav', { class: 'picker-jumps', 'aria-label': 'Categories', hidden: true, 'data-testid': 'picker-jumps' });
  const scroller = h('div', { class: 'picker-scroll' }, list, empty);
  const rows = createIncrementalRenderer(list);

  dlg.append(
    h(
      'div',
      { class: 'sheet-head' },
      h('div', {}, h('h2', { id: 'picker-title', class: 'sheet-title' }, title), subtitle ? h('p', { class: 'sheet-sub' }, subtitle) : null),
      closeBtn,
    ),
    h('div', { class: 'picker-search-wrap' }, icon('search', { size: 18 }), search),
    jumps,
    scroller,
  );

  // The organised (unsearched) list never changes while the picker is open.
  const org = organizeTargets(targets, { sourceCategory, formats });
  const groups = [];
  if (org.compress) groups.push({ key: 'compress', label: 'Same format, smaller', items: [org.compress] });
  if (suggest && org.count > 10 && org.suggested.length) groups.push({ key: 'suggested', label: 'Suggested', items: org.suggested });
  for (const g of org.groups) groups.push({ key: g.category, label: g.label, items: g.targets, category: g.category });

  let active = -1;
  const options = () => [...list.querySelectorAll('[role="option"]')];
  const setActive = (i, { scroll = true } = {}) => {
    let all = options();
    if (i >= all.length && rows.busy) {
      rows.flush();
      all = options();
    }
    if (!all.length) {
      active = -1;
      search.removeAttribute('aria-activedescendant');
      return;
    }
    const next = Math.max(0, Math.min(all.length - 1, i));
    if (all[active]) all[active].classList.remove('is-active');
    active = next;
    all[active].classList.add('is-active');
    search.setAttribute('aria-activedescendant', all[active].id);
    if (scroll) all[active].scrollIntoView({ block: 'nearest' });
  };

  const renderGroups = (sections) => {
    const jobs = [];
    const seen = new Set();
    for (const g of sections) {
      const labelId = uniqueId('grp');
      const ul = h('ul', { role: 'group', class: 'picker-group', 'aria-labelledby': labelId });
      // An id appears once per list; suggested duplicates get their own ids.
      const dup = new Set(g.items.filter((t) => seen.has(t.id)).map((t) => t.id));
      for (const t of g.items) seen.add(t.id);
      list.append(
        h(
          'div',
          { class: 'picker-group-wrap', role: 'none', 'data-group': g.key },
          h('p', { class: 'picker-group-label', id: labelId, role: 'presentation' }, g.label),
          ul,
        ),
      );
      jobs.push({ parent: ul, items: g.items, make: (t) => optionRow(t, { current, idPrefix: dup.has(t.id) ? `${idPrefix}-${g.key}` : idPrefix }) });
    }
    return jobs;
  };

  const renderJumps = (visible) => {
    clear(jumps);
    const cats = groups.filter((g) => g.category);
    jumps.hidden = !visible || cats.length < JUMP_MIN_GROUPS;
    if (jumps.hidden) return;
    for (const g of cats) {
      jumps.append(
        h(
          'button',
          {
            type: 'button',
            class: 'chip',
            'data-cat': categoryKey(g.category),
            'data-testid': `picker-jump-${g.category}`,
            tabindex: '-1', // the search field keeps keyboard focus; arrows move through the list
            onClick: () => {
              rows.flush();
              list.querySelector(`[data-group="${CSS.escape(g.key)}"]`)?.scrollIntoView({ block: 'start' });
              const first = list.querySelector(`[data-group="${CSS.escape(g.key)}"] [role="option"]`);
              if (first) setActive(options().indexOf(first), { scroll: false });
              search.focus({ preventScroll: true });
            },
          },
          h('span', { class: 'cat-dot', 'aria-hidden': 'true' }),
          g.label,
        ),
      );
    }
  };

  const render = () => {
    clear(list);
    active = -1;
    const q = search.value.trim();
    let count = 0;
    let jobs;
    if (q) {
      const results = searchEntries(targets, q);
      count = results.length;
      const ul = h('ul', { role: 'group', class: 'picker-group', 'aria-label': 'Results' });
      if (results.length) list.append(ul);
      jobs = [{ parent: ul, items: results, make: (t) => optionRow(t, { current, idPrefix }) }];
    } else {
      jobs = renderGroups(groups);
      count = groups.reduce((n, g) => n + g.items.length, 0);
    }
    renderJumps(!q);
    empty.hidden = count > 0;
    rows.render(jobs);
    if (q) {
      setActive(0);
      return;
    }
    // The current target: usually in the first rows, otherwise build the rest now.
    let at = options().findIndex((o) => o.dataset.value === current);
    if (at < 0 && current && rows.busy) {
      rows.flush();
      at = options().findIndex((o) => o.dataset.value === current);
    }
    setActive(Math.max(0, at));
  };

  return new Promise((resolve) => {
    let result = null;
    let settled = false;
    const settle = () => {
      if (settled) return;
      settled = true;
      resolve(result);
    };
    const choose = (value) => {
      result = value;
      dlg.close();
      settle(); // do not wait for the (asynchronous) close event
    };
    const onKey = (e) => {
      const all = options();
      if (e.key === 'ArrowDown') {
        e.preventDefault();
        setActive(active + 1);
      } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        setActive(active - 1);
      } else if (e.key === 'PageDown') {
        e.preventDefault();
        setActive(active + 8);
      } else if (e.key === 'PageUp') {
        e.preventDefault();
        setActive(active - 8);
      } else if ((e.key === 'End' || e.key === 'Home') && e.ctrlKey) {
        e.preventDefault();
        setActive(e.key === 'End' ? Number.MAX_SAFE_INTEGER : 0);
      } else if (e.key === 'Enter') {
        e.preventDefault();
        if (all[active]) choose(all[active].dataset.value);
      }
    };
    search.addEventListener('input', render);
    search.addEventListener('keydown', onKey);
    list.addEventListener('click', (e) => {
      const opt = e.target.closest('[role="option"]');
      if (opt) choose(opt.dataset.value);
    });
    list.addEventListener('pointermove', (e) => {
      const opt = e.target.closest('[role="option"]');
      if (opt) {
        const i = options().indexOf(opt);
        if (i !== active) setActive(i, { scroll: false });
      }
    });
    closeBtn.addEventListener('click', () => dlg.close());
    dlg.addEventListener('close', () => rows.cancel(), { once: true });
    dlg.onclick = (e) => {
      if (e.target === dlg) dlg.close(); // backdrop
    };
    dlg.addEventListener('close', settle, { once: true });
    render();
    dlg.showModal();
    // Focus the search field on devices with a keyboard; on touch screens
    // opening the keyboard immediately would hide half of the list.
    if (matchMedia('(pointer: fine)').matches) search.focus();
    else closeBtn.focus();
  });
}

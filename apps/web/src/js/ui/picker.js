// Convertible web app - the searchable target-format picker (a modal dialog
// with a combobox + listbox, grouped by category).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { categoryKey, organizeTargets, searchEntries } from '../model/targets.js';
import { formatLabel } from '../model/format.js';
import { clear, h, icon, uniqueId } from './dom.js';

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
 */
export function openPicker({ title, subtitle = '', targets, current = null, sourceCategory = null, formats = new Map() }) {
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

  dlg.append(
    h(
      'div',
      { class: 'sheet-head' },
      h('div', {}, h('h2', { id: 'picker-title', class: 'sheet-title' }, title), subtitle ? h('p', { class: 'sheet-sub' }, subtitle) : null),
      closeBtn,
    ),
    h('div', { class: 'picker-search-wrap' }, icon('search', { size: 18 }), search),
    h('div', { class: 'picker-scroll' }, list, empty),
  );

  let active = -1;
  const options = () => [...list.querySelectorAll('[role="option"]')];
  const setActive = (i) => {
    const all = options();
    if (!all.length) {
      active = -1;
      search.removeAttribute('aria-activedescendant');
      return;
    }
    active = Math.max(0, Math.min(all.length - 1, i));
    all.forEach((o, n) => o.classList.toggle('is-active', n === active));
    search.setAttribute('aria-activedescendant', all[active].id);
    all[active].scrollIntoView({ block: 'nearest' });
  };

  const render = () => {
    clear(list);
    const q = search.value.trim();
    let count = 0;
    if (q) {
      const results = searchEntries(targets, q);
      count = results.length;
      if (results.length) {
        list.append(h('ul', { role: 'group', class: 'picker-group', 'aria-label': 'Results' }, results.map((t) => optionRow(t, { current, idPrefix }))));
      }
    } else {
      const org = organizeTargets(targets, { sourceCategory, formats });
      const groups = [];
      if (org.compress) groups.push({ key: 'compress', label: 'Same format, smaller', items: [org.compress] });
      if (org.count > 10 && org.suggested.length) groups.push({ key: 'suggested', label: 'Suggested', items: org.suggested });
      for (const g of org.groups) groups.push({ key: g.category, label: g.label, items: g.targets });
      const seen = new Set();
      for (const g of groups) {
        const labelId = uniqueId('grp');
        const items = g.items.map((t) => {
          // An id appears once per list; suggested duplicates get their own ids.
          const row = optionRow(t, { current, idPrefix: seen.has(t.id) ? `${idPrefix}-${g.key}` : idPrefix });
          seen.add(t.id);
          return row;
        });
        count += items.length;
        list.append(
          h('div', { class: 'picker-group-wrap', role: 'none' }, h('p', { class: 'picker-group-label', id: labelId, role: 'presentation' }, g.label), h('ul', { role: 'group', class: 'picker-group', 'aria-labelledby': labelId }, items)),
        );
      }
    }
    empty.hidden = count > 0;
    const all = options();
    const currentIndex = q ? 0 : Math.max(0, all.findIndex((o) => o.dataset.value === current));
    setActive(currentIndex);
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
        if (i !== active) {
          active = i;
          options().forEach((o, n) => o.classList.toggle('is-active', n === active));
          search.setAttribute('aria-activedescendant', opt.id);
        }
      }
    });
    closeBtn.addEventListener('click', () => dlg.close());
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

// Convertible web app - the Formats browser, Settings/About and Licences
// sheets (native modal dialogs).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { catalogueCategories, catalogueGroups, categoryKey } from '../model/targets.js';
import { formatLabel, plural } from '../model/format.js';
import { clear, h, icon, uniqueId } from './dom.js';
import { createIncrementalRenderer } from './incremental.js';
import { staticMark } from './motion.js';

function sheet(id, title, testid) {
  const titleId = `${id}-title`;
  const closeBtn = h('button', { type: 'button', class: 'icon-btn', 'aria-label': 'Close', 'data-testid': `${testid}-close` }, icon('close'));
  const body = h('div', { class: 'sheet-body' });
  const dlg = h(
    'dialog',
    { class: 'sheet sheet-large', id, 'aria-labelledby': titleId, 'data-testid': testid },
    h('div', { class: 'sheet-head' }, h('h2', { id: titleId, class: 'sheet-title' }, title), closeBtn),
    body,
  );
  closeBtn.addEventListener('click', () => dlg.close());
  dlg.addEventListener('click', (e) => {
    if (e.target === dlg) dlg.close();
  });
  document.body.append(dlg);
  return { dlg, body };
}

// ---------------------------------------------------------------------------
// Formats browser
// ---------------------------------------------------------------------------

export function createFormatsSheet() {
  const { dlg, body } = sheet('formats-sheet', 'Supported formats', 'formats-dialog');
  let formats = [];
  const summary = h('p', { class: 'sheet-sub', 'aria-live': 'polite' });
  const search = h('input', { type: 'search', class: 'picker-search', placeholder: 'Search by name or extension', 'aria-label': 'Search formats', 'data-testid': 'formats-search', autocomplete: 'off', spellcheck: 'false' });
  const filters = h('div', { class: 'chips', role: 'group', 'aria-label': 'Show category', 'data-testid': 'formats-categories' });
  const results = h('div', { class: 'formats-results', 'data-testid': 'formats-results' });
  const rows = createIncrementalRenderer(results, { first: 80, batch: 200 });
  let category = null; // null = every category
  let totals = { readable: 0, writable: 0 };
  body.append(
    h('div', { class: 'formats-intro' }, h('p', {}, 'Everything this version can read and write. Any file can also be turned into Base64, hex, checksums and the other encodings.'), summary),
    h('div', { class: 'picker-search-wrap' }, icon('search', { size: 18 }), search),
    filters,
    results,
  );

  const row = (f) =>
    h(
      'li',
      { class: 'format-row', 'data-testid': `format-${f.id}` },
      h('span', { class: 'badge badge-round', 'data-cat': categoryKey(f.category) }, h('span', { class: 'badge-dot', 'aria-hidden': 'true' }), formatLabel(f.id)),
      h('span', { class: 'format-name' }, h('span', { class: 'format-title', title: f.name }, f.name), h('span', { class: 'format-ext' }, (f.extensions || []).map((e) => `.${e}`).join(' '))),
      h(
        'span',
        { class: 'rw' },
        h('span', { class: ['rw-pill', f.readable ? 'on' : 'off'], title: f.readable ? 'Can be read' : 'Not read yet' }, f.readable ? icon('check', { size: 14 }) : null, 'Read'),
        h('span', { class: ['rw-pill', f.writable ? 'on' : 'off'], title: f.writable ? 'Can be written' : 'Not written yet' }, f.writable ? icon('check', { size: 14 }) : null, 'Write'),
      ),
    );

  const renderFilters = () => {
    clear(filters);
    const chip = (value, label, count) =>
      h(
        'button',
        {
          type: 'button',
          class: 'chip',
          'aria-pressed': category === value ? 'true' : 'false',
          'data-cat': value ? categoryKey(value) : null,
          'data-testid': `formats-category-${value ?? 'all'}`,
          onClick: () => {
            category = category === value ? null : value;
            renderFilters();
            render();
          },
        },
        value ? h('span', { class: 'cat-dot', 'aria-hidden': 'true' }) : null,
        label,
        h('span', { class: 'count' }, String(count)),
      );
    filters.append(chip(null, 'All', formats.length), catalogueCategories(formats).map((c) => chip(c.category, c.label, c.count)));
  };

  const render = () => {
    clear(results);
    const q = search.value.trim();
    const groups = catalogueGroups(formats, q, { category });
    const count = groups.reduce((a, g) => a + g.formats.length, 0);
    summary.textContent =
      q || category
        ? `${plural(count, 'match', 'matches')}`
        : `${plural(formats.length, 'format')} in ${plural(groups.length, 'category', 'categories')}. ${totals.readable} readable, ${totals.writable} writable.`;
    if (!groups.length) {
      rows.cancel();
      results.append(h('p', { class: 'picker-empty' }, 'No matching formats.'));
      return;
    }
    const jobs = [];
    for (const g of groups) {
      const headingId = uniqueId('fg');
      const ul = h('ul', { class: 'format-list' });
      results.append(
        h(
          'section',
          { class: 'format-group', 'aria-labelledby': headingId, 'data-cat': categoryKey(g.category) },
          h('h3', { id: headingId, class: 'format-group-title' }, h('span', { class: 'cat-dot', 'aria-hidden': 'true' }), g.label, h('span', { class: 'count' }, ` ${g.formats.length}`)),
          ul,
        ),
      );
      jobs.push({ parent: ul, items: g.formats, make: row });
    }
    rows.render(jobs);
  };
  // Typing restarts the (incremental) render once per burst of input events.
  let pendingInput = 0;
  search.addEventListener('input', () => {
    if (pendingInput) return;
    pendingInput = setTimeout(() => {
      pendingInput = 0;
      render();
    });
  });
  dlg.addEventListener('close', () => rows.cancel());

  return {
    setFormats(list) {
      formats = list || [];
      totals = { readable: formats.filter((f) => f.readable).length, writable: formats.filter((f) => f.writable).length };
      if (dlg.open) {
        renderFilters();
        render();
      }
    },
    open() {
      search.value = '';
      category = null;
      renderFilters();
      render();
      dlg.showModal();
      results.scrollTop = 0;
      if (matchMedia('(pointer: fine)').matches) search.focus();
    },
  };
}

// ---------------------------------------------------------------------------
// Settings & About
// ---------------------------------------------------------------------------

function radioGroup(legend, name, choices, current, onChange, testid) {
  return h(
    'fieldset',
    { class: 'segmented', 'data-testid': testid },
    h('legend', { class: 'field-label' }, legend),
    h(
      'div',
      { class: 'segmented-track' },
      choices.map((c) =>
        h(
          'label',
          { class: 'segment' },
          h('input', { type: 'radio', name, value: c.value, checked: c.value === current, 'data-testid': `${testid}-${c.value}`, onChange: () => onChange(c.value) }),
          h('span', {}, c.label),
        ),
      ),
    ),
  );
}

export function createSettingsSheet({ getSettings, onChange, versions, openLicences }) {
  const { dlg, body } = sheet('settings-sheet', 'Settings', 'settings-dialog');
  const render = () => {
    clear(body);
    const st = getSettings();
    const stripId = uniqueId('strip');
    const v = versions();
    body.append(
      h(
        'section',
        { class: 'settings-section', 'aria-label': 'Preferences' },
        radioGroup('Theme', uniqueId('theme'), [{ value: 'system', label: 'System' }, { value: 'dark', label: 'Dark' }, { value: 'light', label: 'Light' }], st.theme, (theme) => onChange({ theme }), 'theme'),
        radioGroup('Default quality', uniqueId('preset'), [{ value: 'high', label: 'High' }, { value: 'balanced', label: 'Balanced' }, { value: 'small', label: 'Small' }], st.preset, (preset) => onChange({ preset }), 'default-preset'),
        h(
          'div',
          { class: 'field field-switch' },
          h('label', { for: stripId, class: 'field-label' }, 'Remove metadata'),
          h('input', { id: stripId, type: 'checkbox', role: 'switch', class: 'switch', checked: st.stripMetadata, 'data-testid': 'strip-metadata', 'aria-describedby': `${stripId}-help`, onChange: (e) => onChange({ stripMetadata: e.target.checked }) }),
          h('p', { class: 'field-help', id: `${stripId}-help` }, 'Strips camera, location and author details from converted files where the format carries them.'),
        ),
      ),
      h(
        'section',
        { class: 'settings-section privacy-panel', 'aria-labelledby': 'privacy-h' },
        h('h3', { id: 'privacy-h', class: 'settings-h' }, icon('lock', { size: 18 }), 'Privacy'),
        h('p', {}, 'Your files never leave this device. Convertible runs entirely in your browser: nothing is uploaded, there are no accounts, no analytics and no tracking, and it keeps working offline once loaded.'),
        h('p', {}, 'Conversions you make are kept in memory only and disappear when you close the page. The only thing stored is the three preferences above.'),
      ),
      h(
        'section',
        { class: 'settings-section about', 'aria-labelledby': 'about-h' },
        h('h3', { id: 'about-h', class: 'settings-h' }, 'About'),
        h('div', { class: 'about-row' }, staticMark({ size: 40 }), h('div', {}, h('p', { class: 'about-name' }, 'Convertible'), h('p', { class: 'about-tag' }, 'Open-source, offline file converter'))),
        h(
          'dl',
          { class: 'versions' },
          h('dt', {}, 'App version'),
          h('dd', { 'data-testid': 'app-version' }, v.app),
          h('dt', {}, 'Core version'),
          h('dd', { 'data-testid': 'core-version' }, v.core || '…'),
          h('dt', {}, 'Formats'),
          h('dd', {}, v.formats ? String(v.formats) : '…'),
        ),
        h('button', { type: 'button', class: 'btn btn-secondary', 'data-testid': 'open-licences', onClick: openLicences }, h('span', {}, 'Open-source licences')),
      ),
    );
  };
  return {
    open() {
      render();
      dlg.showModal();
    },
    refresh() {
      if (dlg.open) render();
    },
  };
}

// ---------------------------------------------------------------------------
// Licences
// ---------------------------------------------------------------------------

const NOTICES = [
  { title: 'Convertible', subtitle: 'MIT licence', file: 'legal/LICENSE.txt' },
  { title: 'Inter typeface', subtitle: 'SIL Open Font License 1.1', file: 'legal/Inter-LICENSE.txt' },
  { title: 'JetBrains Mono typeface', subtitle: 'SIL Open Font License 1.1', file: 'legal/JetBrainsMono-LICENSE.txt' },
  { title: 'Third-party components', subtitle: 'Codecs and fonts compiled into the converter, each under its own permissive licence', file: 'legal/NOTICES.txt' },
];

export function createLicencesSheet() {
  const { dlg, body } = sheet('licences-sheet', 'Open-source licences', 'licences-dialog');
  let loaded = false;
  return {
    async open() {
      if (!loaded) {
        clear(body);
        body.append(h('p', { class: 'sheet-sub' }, 'Convertible is free software. It includes the following components, each under its own licence.'));
        for (const n of NOTICES) {
          let text = '';
          try {
            text = await (await fetch(n.file)).text();
          } catch {
            text = 'The licence text is not available offline yet.';
          }
          body.append(h('details', { class: 'licence' }, h('summary', {}, h('span', { class: 'licence-title' }, n.title), h('span', { class: 'licence-sub' }, n.subtitle)), h('pre', { class: 'licence-text' }, text)));
        }
        loaded = true;
      }
      dlg.showModal();
    },
  };
}

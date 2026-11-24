// Convertible web app - target lists (cvt_targets_json) and the format
// catalogue (cvt_formats_json): grouping, ordering and search. Pure, no DOM.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

export const CATEGORY_LABELS = Object.freeze({
  image: 'Images',
  audio: 'Audio',
  video: 'Video',
  document: 'Documents',
  ebook: 'E-books',
  spreadsheet: 'Spreadsheets',
  presentation: 'Presentations',
  data: 'Data',
  archive: 'Archives',
  font: 'Fonts',
  subtitle: 'Subtitles',
  model3d: '3D models',
  geo: 'Maps & GPS',
  pim: 'Calendars & contacts',
  code: 'Code',
  encoding: 'Encodings & checksums',
  vector: 'Vector graphics',
  other: 'Other',
});

/** Canonical category order, used when popularity ties. */
export const CATEGORY_ORDER = Object.freeze(Object.keys(CATEGORY_LABELS));

export function categoryLabel(category) {
  return CATEGORY_LABELS[category] ?? (category ? category[0].toUpperCase() + category.slice(1) : 'Other');
}

/** Safe CSS identifier for a category (falls back to "other"). */
export function categoryKey(category) {
  return CATEGORY_LABELS[category] ? category : 'other';
}

/** id -> format entry from cvt_formats_json. */
export function indexFormats(formats) {
  const map = new Map();
  for (const f of formats || []) map.set(f.id, f);
  return map;
}

/**
 * Orders and groups the targets of one source format for the picker.
 *
 * - "Suggested": up to `suggested` targets from the source's own category and
 *   the most popular formats, in the core's relevance order.
 * - Then one group per category: the source's category first, then
 *   categories by their most popular member. Inside a group the core's
 *   relevance order is kept.
 * - The "compress" entry (same format) is reported separately.
 */
export function organizeTargets(targets, { sourceCategory = null, formats = new Map(), suggested = 6 } = {}) {
  const list = (targets || []).filter((t) => t && t.id);
  const popularity = (t) => formats.get(t.id)?.popularity ?? 0;
  const compress = list.find((t) => t.compress) || null;
  const regular = list.filter((t) => !t.compress);

  const byCategory = new Map();
  for (const t of regular) {
    const c = t.category || 'other';
    if (!byCategory.has(c)) byCategory.set(c, []);
    byCategory.get(c).push(t);
  }
  const groups = [...byCategory.entries()].map(([category, items]) => ({
    category,
    label: categoryLabel(category),
    targets: items,
    top: Math.max(...items.map(popularity)),
  }));
  groups.sort((a, b) => {
    if (a.category === sourceCategory) return -1;
    if (b.category === sourceCategory) return 1;
    if (b.top !== a.top) return b.top - a.top;
    return CATEGORY_ORDER.indexOf(a.category) - CATEGORY_ORDER.indexOf(b.category);
  });

  const picks = [];
  const same = regular.filter((t) => t.category === sourceCategory);
  for (const t of same) if (picks.length < suggested) picks.push(t);
  const popular = regular
    .filter((t) => !picks.includes(t))
    .map((t, i) => ({ t, i }))
    .sort((a, b) => popularity(b.t) - popularity(a.t) || a.i - b.i);
  for (const { t } of popular) if (picks.length < suggested) picks.push(t);
  const order = new Map(regular.map((t, i) => [t.id, i]));
  picks.sort((a, b) => order.get(a.id) - order.get(b.id));

  return {
    compress,
    suggested: picks,
    groups: groups.map(({ top, ...g }) => g),
    count: regular.length,
  };
}

/** The target to preselect: the first same-category target, else the first target. */
export function defaultTarget(targets, sourceCategory) {
  const regular = (targets || []).filter((t) => !t.compress);
  return regular.find((t) => t.category === sourceCategory) || regular[0] || null;
}

/** Targets offered by the "Compress" action: the same-format entry and archive formats. */
export function compressTargets(targets) {
  return (targets || []).filter((t) => t.compress || t.category === 'archive');
}

function scoreEntry(entry, q) {
  const id = entry.id.toLowerCase();
  const name = (entry.name || '').toLowerCase();
  const exts = (entry.extensions || []).map((e) => e.toLowerCase());
  if (id === q || exts.includes(q)) return 100;
  if (id.startsWith(q)) return 80;
  if (exts.some((e) => e.startsWith(q))) return 70;
  if (name.startsWith(q)) return 60;
  if (name.split(/[\s/()-]+/).some((w) => w.startsWith(q))) return 50;
  if (id.includes(q) || name.includes(q)) return 30;
  if ((entry.category || '').startsWith(q) || categoryLabel(entry.category).toLowerCase().startsWith(q)) return 10;
  if ((entry.description || '').toLowerCase().includes(q) || (entry.mime || '').toLowerCase().includes(q)) return 5;
  return 0;
}

/**
 * Searches targets or catalogue entries by id, extension, name, category,
 * description and MIME type. Every word of the query must match. Results are
 * ordered by match quality, then by their original order.
 */
export function searchEntries(entries, query) {
  const words = String(query || '').toLowerCase().replace(/^\./, '').split(/\s+/).filter(Boolean);
  const list = entries || [];
  if (!words.length) return list.slice();
  const scored = [];
  list.forEach((entry, index) => {
    let total = 0;
    for (const w of words) {
      const s = scoreEntry(entry, w);
      if (!s) return;
      total += s;
    }
    scored.push({ entry, index, total });
  });
  scored.sort((a, b) => b.total - a.total || a.index - b.index);
  return scored.map((s) => s.entry);
}

/**
 * Groups the catalogue for the formats browser:
 * [{category, label, formats, readable, writable}] in canonical order, each
 * group most popular first (the catalogue's order).
 */
export function catalogueGroups(formats, query = '') {
  const filtered = searchEntries(formats, query);
  const map = new Map();
  for (const f of filtered) {
    const c = f.category || 'other';
    if (!map.has(c)) map.set(c, []);
    map.get(c).push(f);
  }
  const rank = (c) => {
    const i = CATEGORY_ORDER.indexOf(c);
    return i < 0 ? CATEGORY_ORDER.length : i;
  };
  return [...map.entries()]
    .sort((a, b) => rank(a[0]) - rank(b[0]))
    .map(([category, list]) => ({
      category,
      label: categoryLabel(category),
      formats: query ? list : list.slice().sort((a, b) => (b.popularity ?? 0) - (a.popularity ?? 0)),
      readable: list.filter((f) => f.readable).length,
      writable: list.filter((f) => f.writable).length,
    }));
}

/** Extensions accepted by the file picker / file handlers: every readable format. */
export function acceptedExtensions(formats) {
  const set = new Set();
  for (const f of formats || []) if (f.readable) for (const e of f.extensions || []) set.add(`.${e.toLowerCase()}`);
  return [...set].sort();
}

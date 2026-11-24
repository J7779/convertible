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

/**
 * Targets offered by the "Compress" action: the same-format entry and archive
 * formats. Newer cores mark targets with a `role` ("compress", "archive",
 * "extract"); when present it decides, otherwise the archive category does.
 */
export function compressTargets(targets) {
  const list = targets || [];
  if (list.some((t) => t.role)) return list.filter((t) => t.compress || t.role === 'compress' || t.role === 'archive');
  return list.filter((t) => t.compress || (t.category === 'archive' && t.role !== 'extract'));
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
  const words = String(query || '')
    .toLowerCase()
    .split(/\s+/)
    .map((w) => w.replace(/^\.+/, ''))
    .filter(Boolean);
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

function categoryRank(c) {
  const i = CATEGORY_ORDER.indexOf(c);
  return i < 0 ? CATEGORY_ORDER.length : i;
}

/**
 * Groups the catalogue for the formats browser:
 * [{category, label, formats, readable, writable}]. Without a query the
 * groups come in canonical order, each most popular first; with a query the
 * group holding the best match comes first and each group keeps the match
 * order. `category` (optional) keeps that category only.
 */
export function catalogueGroups(formats, query = '', { category = null } = {}) {
  const pool = category ? (formats || []).filter((f) => (f.category || 'other') === category) : formats;
  const filtered = searchEntries(pool, query);
  const map = new Map(); // insertion order = order of each category's best match
  for (const f of filtered) {
    const c = f.category || 'other';
    if (!map.has(c)) map.set(c, []);
    map.get(c).push(f);
  }
  const entries = [...map.entries()];
  if (!String(query || '').trim()) entries.sort((a, b) => categoryRank(a[0]) - categoryRank(b[0]));
  return entries.map(([cat, list]) => ({
    category: cat,
    label: categoryLabel(cat),
    formats: query ? list : list.slice().sort((a, b) => (b.popularity ?? 0) - (a.popularity ?? 0)),
    readable: list.filter((f) => f.readable).length,
    writable: list.filter((f) => f.writable).length,
  }));
}

/** [{category, label, count}] for every category present, in canonical order. */
export function catalogueCategories(formats) {
  const counts = new Map();
  for (const f of formats || []) {
    const c = f.category || 'other';
    counts.set(c, (counts.get(c) ?? 0) + 1);
  }
  return [...counts.entries()]
    .sort((a, b) => categoryRank(a[0]) - categoryRank(b[0]))
    .map(([category, count]) => ({ category, label: categoryLabel(category), count }));
}

/**
 * Chromium accepts at most 300 file extensions across all of a manifest's
 * file handlers and ignores the rest with a manifest error.
 */
export const FILE_HANDLER_EXTENSION_LIMIT = 300;

const MIME_RE = /^[a-z0-9][a-z0-9!#$&^_.+-]*\/[a-z0-9][a-z0-9!#$&^_.+-]*$/i;

/**
 * The manifest's file_handlers: one handler per category (in canonical
 * order), each accepting its formats' extensions grouped by MIME type. The
 * most popular readable formats are taken until the browser's limit; a
 * format's extensions are either all in or all out. Files of the other
 * formats still open through the file chooser, drag and drop, and sharing.
 */
export function fileHandlers(formats, { limit = FILE_HANDLER_EXTENSION_LIMIT, icons = [] } = {}) {
  const readable = (formats || []).filter((f) => f.readable && f.extensions?.length);
  readable.sort((a, b) => (b.popularity ?? 0) - (a.popularity ?? 0)); // stable: catalogue order on ties
  const taken = new Set();
  const byCategory = new Map();
  for (const f of readable) {
    const fresh = [...new Set(f.extensions.map((e) => `.${String(e).toLowerCase()}`))].filter((e) => !taken.has(e));
    if (!fresh.length || taken.size + fresh.length > limit) continue;
    for (const e of fresh) taken.add(e);
    const category = f.category || 'other';
    const mime = MIME_RE.test(f.mime || '') ? f.mime.toLowerCase() : 'application/octet-stream';
    if (!byCategory.has(category)) byCategory.set(category, new Map());
    const accept = byCategory.get(category);
    accept.set(mime, [...(accept.get(mime) ?? []), ...fresh]);
  }
  return [...byCategory.entries()]
    .sort((a, b) => categoryRank(a[0]) - categoryRank(b[0]))
    .map(([category, accept]) => ({
      action: './',
      name: categoryLabel(category),
      accept: Object.fromEntries([...accept.entries()].map(([mime, exts]) => [mime, exts.sort()])),
      ...(icons.length ? { icons } : {}),
    }));
}

/** Extensions accepted by the file picker: every readable format. */
export function acceptedExtensions(formats) {
  const set = new Set();
  for (const f of formats || []) if (f.readable) for (const e of f.extensions || []) set.add(`.${e.toLowerCase()}`);
  return [...set].sort();
}

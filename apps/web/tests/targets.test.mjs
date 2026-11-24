// Target ordering, grouping and search tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { importSrc, loadCore } from './helpers.mjs';

const { organizeTargets, defaultTarget, compressTargets, searchEntries, catalogueGroups, catalogueCategories, fileHandlers, indexFormats, acceptedExtensions, categoryLabel, categoryKey } =
  await importSrc('js/model/targets.js');

const formats = indexFormats([
  { id: 'png', name: 'PNG image', category: 'image', extensions: ['png'], popularity: 95, readable: true, writable: true },
  { id: 'jpg', name: 'JPEG image', category: 'image', extensions: ['jpg', 'jpeg'], popularity: 98, readable: true, writable: true },
  { id: 'webp', name: 'WebP image', category: 'image', extensions: ['webp'], popularity: 80, readable: true, writable: true },
  { id: 'pdf', name: 'PDF document', category: 'document', extensions: ['pdf'], popularity: 99, readable: true, writable: true },
  { id: 'zip', name: 'ZIP archive', category: 'archive', extensions: ['zip'], popularity: 90, readable: true, writable: true },
  { id: 'base64', name: 'Base64 text', category: 'encoding', extensions: ['b64'], popularity: 50, readable: true, writable: true },
  { id: 'hex', name: 'Hexadecimal text', category: 'encoding', extensions: ['hex'], popularity: 40, readable: true, writable: true },
]);
const t = (id, extra = {}) => ({ ...formats.get(id), lossy: false, compress: false, ...extra });
// The core's relevance order for a PNG source.
const pngTargets = [t('png', { compress: true }), t('webp'), t('jpg', { lossy: true }), t('pdf'), t('zip'), t('base64'), t('hex')];

test('groups: source category first, then by popularity; compress separated', () => {
  const org = organizeTargets(pngTargets, { sourceCategory: 'image', formats });
  assert.equal(org.compress.id, 'png');
  assert.equal(org.count, 6);
  assert.deepEqual(org.groups.map((g) => g.category), ['image', 'document', 'archive', 'encoding']);
  assert.deepEqual(org.groups[0].targets.map((x) => x.id), ['webp', 'jpg'], 'core order is kept inside a group');
  assert.equal(org.groups[0].label, 'Images');
});

test('suggested: same category and popular formats, in relevance order', () => {
  const org = organizeTargets(pngTargets, { sourceCategory: 'image', formats, suggested: 4 });
  assert.deepEqual(org.suggested.map((x) => x.id), ['webp', 'jpg', 'pdf', 'zip']);
});

test('default and compress targets', () => {
  assert.equal(defaultTarget(pngTargets, 'image').id, 'webp');
  assert.equal(defaultTarget([t('hex')], 'image').id, 'hex');
  assert.equal(defaultTarget([], 'image'), null);
  assert.deepEqual(compressTargets(pngTargets).map((x) => x.id), ['png', 'zip']);
  // Newer cores: the role decides (an extract target in the archive category is not offered).
  const roled = [t('png', { compress: true, role: 'compress' }), t('zip', { role: 'archive' }), { ...t('zip'), id: 'unzip', role: 'extract' }, t('pdf')];
  assert.deepEqual(compressTargets(roled).map((x) => x.id), ['png', 'zip']);
});

test('search: ids, extensions, names, categories; all words must match', () => {
  assert.deepEqual(searchEntries(pngTargets, 'jpeg').map((x) => x.id), ['jpg']);
  assert.deepEqual(searchEntries(pngTargets, '.JPG').map((x) => x.id), ['jpg']);
  assert.equal(searchEntries(pngTargets, 'image')[0].id, 'png');
  assert.deepEqual(searchEntries(pngTargets, 'text hex').map((x) => x.id), ['hex']);
  assert.deepEqual(searchEntries(pngTargets, 'zzz'), []);
  assert.equal(searchEntries(pngTargets, '  ').length, pngTargets.length);
  // Exact id beats a prefix match on another entry.
  const r = searchEntries([t('base64'), t('hex')], 'hex');
  assert.equal(r[0].id, 'hex');
});

test('catalogue groups and accepted extensions', () => {
  const groups = catalogueGroups([...formats.values()]);
  assert.deepEqual(groups.map((g) => g.category), ['image', 'document', 'archive', 'encoding']);
  assert.deepEqual(groups[0].formats.map((f) => f.id), ['jpg', 'png', 'webp'], 'most popular first');
  assert.equal(groups[0].readable, 3);
  assert.deepEqual(catalogueGroups([...formats.values()], 'zip').map((g) => g.category), ['archive']);
  // Searching: the group holding the best match comes first, not the canonical first group.
  assert.deepEqual(catalogueGroups([...formats.values()], 'pdf').map((g) => g.category)[0], 'document');
  assert.deepEqual(catalogueGroups([...formats.values()], '', { category: 'encoding' }).map((g) => g.formats.map((f) => f.id)), [['base64', 'hex']]);
  assert.deepEqual(catalogueCategories([...formats.values()]).map((c) => [c.category, c.count]), [['image', 3], ['document', 1], ['archive', 1], ['encoding', 2]]);
  assert.ok(acceptedExtensions([...formats.values()]).includes('.jpeg'));
  assert.equal(categoryLabel('model3d'), '3D models');
  assert.equal(categoryLabel('newcat'), 'Newcat');
  assert.equal(categoryKey('newcat'), 'other');
});

test('file handlers: popular formats first, grouped by category and MIME, within the limit', () => {
  const list = [...formats.values(), { id: 'raw', name: 'Raw', category: 'image', extensions: ['raw', 'png'], mime: 'not a mime', popularity: 1, readable: true }];
  const handlers = fileHandlers(list, { limit: 6 });
  const all = handlers.flatMap((h) => Object.values(h.accept).flat());
  assert.ok(all.length <= 6);
  assert.equal(new Set(all).size, all.length, 'no extension twice');
  // Most popular first: pdf (99), jpg+jpeg (98), png (95), zip (90); webp (80) still fits; nothing else does.
  assert.deepEqual([...all].sort(), ['.jpeg', '.jpg', '.pdf', '.png', '.webp', '.zip']);
  assert.deepEqual(handlers.map((h) => h.name), ['Images', 'Documents', 'Archives'], 'canonical category order');
  // Grouped by MIME type; missing or invalid MIME types fall back to application/octet-stream.
  const typed = list.map((f) => (f.id === 'png' ? { ...f, mime: 'image/png' } : f.id === 'jpg' ? { ...f, mime: 'image/jpeg' } : f));
  const images = fileHandlers(typed).find((h) => h.name === 'Images');
  assert.deepEqual(Object.keys(images.accept).sort(), ['application/octet-stream', 'image/jpeg', 'image/png']);
  assert.deepEqual(images.accept['image/jpeg'], ['.jpeg', '.jpg']);
  assert.deepEqual(images.accept['application/octet-stream'].sort(), ['.raw', '.webp']);
});

test('the real catalogue: every source organizes without loss', async () => {
  const core = await loadCore();
  const all = core.formats();
  const index = indexFormats(all);
  for (const f of all.filter((x) => x.readable)) {
    const targets = core.targets(f.id);
    const org = organizeTargets(targets, { sourceCategory: f.category, formats: index });
    const grouped = org.groups.flatMap((g) => g.targets.map((x) => x.id));
    assert.equal(grouped.length + (org.compress ? 1 : 0), targets.length, f.id);
    assert.equal(new Set(grouped).size, grouped.length);
    for (const s of org.suggested) assert.ok(grouped.includes(s.id));
    for (const x of targets) assert.ok(searchEntries(targets, x.id).includes(x), `${x.id} is findable by id`);
  }
  const groups = catalogueGroups(all);
  assert.equal(groups.reduce((a, g) => a + g.formats.length, 0), all.length);
});

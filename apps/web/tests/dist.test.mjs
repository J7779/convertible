// Built site tests: service-worker precache consistency, manifest validity,
// no external references, CSP, motion storyboard conversion.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { before, test } from 'node:test';
import { pathToFileURL } from 'node:url';
import { DIST, WEB_DIR, importSrc, requireDist } from './helpers.mjs';

const precache = await import(pathToFileURL(path.join(WEB_DIR, 'tools', 'precache.mjs')).href);

before(() => requireDist());

test('every file in dist is precached, and nothing else', async () => {
  const files = await precache.listFiles(DIST);
  const sw = await readFile(path.join(DIST, 'sw.js'), 'utf8');
  const { version, urls } = precache.readManifestFromSw(sw);
  assert.match(version, /^[0-9a-f]{12}$/);
  const expected = files.filter((f) => !precache.PRECACHE_EXCLUDE.has(path.posix.basename(f))).map((f) => `./${f}`);
  assert.deepEqual([...urls].sort(), expected.sort());
  for (const must of ['./index.html', './wasm/convertible.wasm', './wasm/convertible.mjs', './js/core/worker.js', './manifest.webmanifest', './css/app.css']) {
    assert.ok(urls.includes(must), must);
  }
});

test('the cache version is a digest of the content', async () => {
  const sw = await readFile(path.join(DIST, 'sw.js'), 'utf8');
  const manifest = await precache.buildManifest(DIST);
  assert.equal(precache.readManifestFromSw(sw).version, manifest.version);
  const template = await readFile(path.join(WEB_DIR, 'src', 'sw.js'), 'utf8');
  const changed = { ...manifest, version: 'abcdefabcdef', files: manifest.files.slice(0, 2) };
  const out = precache.injectManifest(template, changed);
  assert.deepEqual(precache.readManifestFromSw(out), { version: 'abcdefabcdef', urls: changed.files.map((f) => f.url) });
  assert.throws(() => precache.injectManifest('no markers', manifest));
});

test('every module and asset referenced by the app exists in dist', async () => {
  const files = new Set(await precache.listFiles(DIST));
  const js = [...files].filter((f) => f.endsWith('.js') && f.startsWith('js/'));
  for (const f of js) {
    const source = await readFile(path.join(DIST, f), 'utf8');
    for (const m of source.matchAll(/(?:from|import)\s*\(?\s*['"](\.{1,2}\/[^'"]+)['"]/g)) {
      const target = path.posix.normalize(path.posix.join(path.posix.dirname(f), m[1]));
      assert.ok(files.has(target), `${f} imports missing ${target}`);
    }
  }
  const html = await readFile(path.join(DIST, 'index.html'), 'utf8');
  for (const m of html.matchAll(/(?:href|src)="([^"#:]+)"/g)) {
    if (m[1] === './') continue;
    assert.ok(files.has(m[1]), `index.html references missing ${m[1]}`);
  }
  const tokens = await readFile(path.join(DIST, 'assets', 'brand', 'tokens.css'), 'utf8');
  for (const m of tokens.matchAll(/url\("([^"]+)"\)/g)) {
    assert.ok(files.has(path.posix.normalize(path.posix.join('assets/brand', m[1]))), m[1]);
  }
});

test('no external requests: no absolute URLs to other hosts in shipped code', async () => {
  const files = await precache.listFiles(DIST);
  for (const f of files.filter((x) => /\.(html|js|mjs|css|webmanifest)$/.test(x))) {
    const source = await readFile(path.join(DIST, f), 'utf8');
    const urls = [...source.matchAll(/https?:\/\/[^\s'"`)<>]+/g)].map((m) => m[0]);
    const external = urls.filter((u) => !u.startsWith('http://www.w3.org/'));
    assert.deepEqual(external, [], `${f} references ${external.join(', ')}`);
  }
});

test('strict Content-Security-Policy', async () => {
  const html = await readFile(path.join(DIST, 'index.html'), 'utf8');
  const csp = /http-equiv="Content-Security-Policy" content="([^"]+)"/.exec(html)?.[1];
  assert.ok(csp);
  assert.match(csp, /default-src 'self'/);
  assert.match(csp, /script-src 'self' 'wasm-unsafe-eval'(;|$)/);
  assert.doesNotMatch(csp, /'unsafe-inline'|'unsafe-eval'|\*|https?:|data:/);
  assert.doesNotMatch(html, /<script(?![^>]*\bsrc=)[^>]*>/, 'no inline scripts');
  assert.doesNotMatch(html, /\sstyle="/, 'no inline styles');
  assert.doesNotMatch(html, /\son[a-z]+="/, 'no inline handlers');
});

test('web app manifest is valid and installable', async () => {
  const m = JSON.parse(await readFile(path.join(DIST, 'manifest.webmanifest'), 'utf8'));
  assert.equal(m.name, 'Convertible');
  assert.equal(m.display, 'standalone');
  assert.equal(m.start_url, './');
  assert.match(m.theme_color, /^#[0-9A-F]{6}$/i);
  const files = new Set(await precache.listFiles(DIST));
  const sizes = (purpose) => m.icons.filter((i) => i.purpose === purpose).map((i) => i.sizes);
  assert.ok(sizes('any').includes('192x192') && sizes('any').includes('512x512'));
  assert.ok(sizes('maskable').includes('512x512'));
  for (const icon of [...m.icons, ...m.shortcuts.flatMap((s) => s.icons || []), ...m.file_handlers.flatMap((h) => h.icons || [])]) {
    assert.ok(files.has(icon.src), icon.src);
  }
  assert.equal(m.share_target.method, 'POST');
  assert.equal(m.share_target.enctype, 'multipart/form-data');
  const exts = Object.values(m.file_handlers[0].accept).flat();
  assert.ok(exts.includes('.txt') && exts.includes('.b64'), 'file handlers come from the readable catalogue');
  for (const e of exts) assert.match(e, /^\.[a-z0-9+._-]+$/);
  assert.ok(m.shortcuts.length >= 2);
});

test('motion storyboard converts to Web Animations keyframes', async () => {
  const storyboard = JSON.parse(await readFile(path.join(WEB_DIR, '..', '..', 'assets', 'brand', 'animation.json'), 'utf8'));
  const { sectionAnimations, cubicBezier, valueAt, trimCss } = await importSrc('js/ui/motion-model.js');
  const ease = cubicBezier([0.65, 0, 0.35, 1]);
  assert.equal(ease(0), 0);
  assert.equal(ease(1), 1);
  assert.ok(Math.abs(ease(0.5) - 0.5) < 1e-4);
  const intro = sectionAnimations(storyboard, 'intro');
  for (const a of intro) {
    assert.equal(a.keyframes[0].offset, 0, `${a.layer}.${a.property} starts at 0`);
    assert.equal(a.keyframes.at(-1).offset, 1, `${a.layer}.${a.property} ends at 1`);
    for (let i = 1; i < a.keyframes.length; i += 1) assert.ok(a.keyframes[i].offset >= a.keyframes[i - 1].offset);
  }
  const trace = intro.find((a) => a.layer === 'trace' && a.property === 'trim');
  assert.equal(trace.keyframes.at(-1).strokeDasharray, '1000 1000', 'the intro ends on the full logo');
  const stage = intro.find((a) => a.layer === 'stage' && a.property === 'scale');
  assert.equal(stage.keyframes.at(-1).scale, '1');
  const spin = sectionAnimations(storyboard, 'spinner').find((a) => a.layer === 'runner');
  const at = (ms) => spin.keyframes.find((k) => Math.abs(k.offset - ms / 1400) < 1e-3);
  assert.equal(at(630).strokeDasharray, '1000 1000');
  assert.equal(at(1400).strokeDasharray, '0 1000');
  assert.equal(at(0).easing, 'cubic-bezier(0.65, 0, 0.35, 1)');
  assert.deepEqual(trimCss(0.25, 0.75), { strokeDasharray: '500 1000', strokeDashoffset: '-250' });
  const track = storyboard.intro.layers.find((l) => l.id === 'seed').tracks[0];
  assert.equal(valueAt(storyboard, track, -5), 0);
  assert.equal(valueAt(storyboard, track, 5000), 1);
});

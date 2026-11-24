// The installable, offline PWA: the manifest, the service worker installing
// and controlling the page with every file precached, a reload and a real
// conversion with the network gone, and the "new version" update flow.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { appendFile, cp, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { appReady, chooseFiles, chooseTarget, convert, openApp, save, tid, waitForReady } from './lib/app.mjs';
import { TEXT, allBytes, tempDir } from './lib/fixtures.mjs';
import { DIST, WEB_DIR } from './lib/paths.mjs';
import { startServer } from './lib/server.mjs';
import { core, e2e, server, suite } from './lib/harness.mjs';

const precache = await import(pathToFileURL(path.join(WEB_DIR, 'tools', 'precache.mjs')).href);
const { acceptedExtensions, fileHandlers, FILE_HANDLER_EXTENSION_LIMIT } = await import('../src/js/model/targets.js');

/** Waits until a service worker controls the page; returns its cache state. */
async function waitForControl(page) {
  await page.waitFor(() => navigator.serviceWorker.controller?.state === 'activated', [], { timeout: 30_000, message: 'the service worker to control the page' });
  return page.evaluate(async () => {
    const names = await caches.keys();
    const entries = {};
    for (const n of names) entries[n] = (await (await caches.open(n)).keys()).map((r) => new URL(r.url).pathname);
    return { names, entries, scope: (await navigator.serviceWorker.getRegistration()).scope };
  });
}

/** Asks the controlling service worker for its cache version. */
function swVersion(page) {
  return page.evaluate(
    () =>
      new Promise((resolve) => {
        const channel = new MessageChannel();
        channel.port1.onmessage = (e) => resolve(e.data);
        navigator.serviceWorker.controller.postMessage({ type: 'version' }, [channel.port2]);
      }),
  );
}

suite('progressive web app', () => {
  e2e('the manifest is linked, valid and installable, and covers every readable format', async ({ ctx }) => {
    const page = await openApp(ctx, server());
    const { url, errors, data } = await page.send('Page.getAppManifest');
    assert.equal(url, `${server().url}manifest.webmanifest`);
    assert.deepEqual(errors, [], 'Chrome parses the manifest without errors');
    const manifest = JSON.parse(data);
    assert.equal(manifest.name, 'Convertible');
    assert.equal(manifest.display, 'standalone');
    assert.equal(new URL(manifest.start_url, url).href, server().url);
    assert.equal(new URL(manifest.scope, url).href, server().url);
    for (const size of ['192x192', '512x512']) assert.ok(manifest.icons.some((i) => i.sizes === size && i.type === 'image/png'), `a ${size} PNG icon`);
    assert.ok(manifest.icons.some((i) => i.purpose === 'maskable'), 'a maskable icon');
    // Every icon and shortcut icon is served as a real image of its declared size.
    const icons = [...manifest.icons, ...(manifest.shortcuts ?? []).flatMap((s) => s.icons ?? [])];
    for (const icon of icons) {
      const info = await page.evaluate(async (src) => {
        const r = await fetch(src);
        const bytes = new Uint8Array(await r.arrayBuffer());
        const view = new DataView(bytes.buffer);
        const png = bytes[0] === 0x89 && bytes[1] === 0x50;
        return { ok: r.ok, type: r.headers.get('content-type'), png, w: png ? view.getUint32(16) : null, h: png ? view.getUint32(20) : null };
      }, icon.src);
      assert.ok(info.ok, icon.src);
      if (icon.type === 'image/png') assert.equal(`${info.w}x${info.h}`, icon.sizes, `${icon.src} is ${icon.sizes}`);
    }
    // File handlers: the most popular readable formats, within the browser's
    // limit on extensions, grouped by category and MIME type.
    const accept = manifest.file_handlers.flatMap((h) => Object.values(h.accept).flat());
    assert.ok(accept.length <= FILE_HANDLER_EXTENSION_LIMIT, `${accept.length} extensions`);
    assert.equal(new Set(accept).size, accept.length, 'each extension once');
    const readable = new Set(acceptedExtensions(core.formats()));
    for (const e of accept) assert.ok(readable.has(e), `${e} is readable`);
    assert.deepEqual(manifest.file_handlers, fileHandlers(core.formats(), { icons: manifest.file_handlers[0].icons }));
    const top = core.formats().filter((f) => f.readable && f.extensions.length).slice(0, 20);
    for (const f of top) assert.ok(f.extensions.some((e) => accept.includes(`.${e}`)), `${f.id} (one of the most popular) opens with the app`);

    await waitForControl(page);
    const installability = await page.send('Page.getInstallabilityErrors');
    // Test contexts are incognito-like, which Chrome itself reports; nothing else may be missing.
    assert.deepEqual(installability.installabilityErrors.map((e) => e.errorId).filter((id) => id !== 'in-incognito'), [], 'installable');
  });

  e2e('the service worker installs, controls the page and precaches every file of the build', async ({ ctx }) => {
    const page = await openApp(ctx, server());
    const sw = await readFile(path.join(DIST, 'sw.js'), 'utf8');
    const { version, urls } = precache.readManifestFromSw(sw);
    const state = await waitForControl(page);
    assert.equal(state.scope, server().url);
    assert.deepEqual(state.names.filter((n) => n.startsWith('convertible-app-')), [`convertible-app-${version}`], 'exactly one app cache, named after the build');
    const cached = state.entries[`convertible-app-${version}`].sort();
    const files = (await precache.listFiles(DIST)).filter((f) => !precache.PRECACHE_EXCLUDE.has(path.posix.basename(f)));
    assert.deepEqual(cached, files.map((f) => `/${f}`).sort(), 'every file of dist is cached (and nothing else)');
    assert.deepEqual(urls.map((u) => u.slice(1)).sort(), files.map((f) => `/${f}`).sort());
    assert.equal(await swVersion(page), version);
    // "Ready to work offline" is announced on the first install.
    await page.waitForText('Ready to work offline.', { selector: '#toasts' });
    // Requests are now answered by the service worker.
    await page.reload();
    await appReady(page);
    const fromSw = await page.evaluate(() => performance.getEntriesByType('resource').filter((e) => e.name.endsWith('.js')).map((e) => e.workerStart > 0 || e.deliveryType === 'cache'));
    assert.ok(fromSw.length > 0 && fromSw.every(Boolean), 'scripts come through the service worker');
  });

  e2e('offline: the app reloads and converts a file with no network and no server', async ({ ctx }) => {
    // A server of its own, so it can be shut down.
    const own = await startServer(DIST);
    ctx.origin = own.origin;
    const files = await tempDir();
    try {
      const page = await openApp(ctx, own);
      await waitForControl(page);
      await own.close();
      await ctx.setOffline(true);
      assert.equal(await page.evaluate(() => navigator.onLine), false);

      await page.reload();
      await appReady(page);
      const input = allBytes();
      const file = await files.file('offline.bin', input);
      await chooseFiles(page, [file]);
      await waitForReady(page, ['offline.bin']);
      await chooseTarget(page, 'offline.bin', 'base64');
      await convert(page, 'offline.bin');
      const d = await save(page, ctx, 'offline.bin', 'offline.b64');
      assert.ok(Buffer.from(d.bytes.toString('latin1'), 'base64').equals(input));
      // The formats browser and licences work offline too (precached).
      await page.click(tid('open-settings'));
      await page.click(tid('open-licences'));
      await page.waitForSelector('dialog[data-testid="licences-dialog"][open]');
      assert.doesNotMatch(await page.text('dialog[data-testid="licences-dialog"]'), /not available offline/);
    } finally {
      await files.remove();
      await own.close();
    }
  }, {
    // Offline, the browser's own update check of sw.js cannot reach the server.
    allow: { failure: (f) => /\/sw\.js$/.test(f.url) && /INTERNET_DISCONNECTED|CONNECTION_REFUSED/.test(f.error) },
  });

  e2e('a new deployment shows the "new version" prompt; Reload switches to it', async ({ ctx }) => {
    // Serve a copy of the build that can be redeployed.
    const site = await tempDir('cvt-e2e-site-');
    await cp(DIST, site.dir, { recursive: true });
    const own = await startServer(site.dir);
    ctx.origin = own.origin;
    try {
      const page = await openApp(ctx, own);
      await waitForControl(page);
      const v1 = await swVersion(page);

      // Deploy v2: a changed stylesheet means a new cache version.
      await appendFile(path.join(site.dir, 'css', 'app.css'), '\n/* deployment 2 */\n');
      const manifest = await precache.buildManifest(site.dir);
      const template = await readFile(path.join(WEB_DIR, 'src', 'sw.js'), 'utf8');
      await writeFile(path.join(site.dir, 'sw.js'), precache.injectManifest(template, manifest));
      assert.notEqual(manifest.version, v1);

      // The user comes back to the app: it finds the update and offers it.
      await page.reload();
      await appReady(page);
      await page.waitForSelector(tid('update-reload'), { timeout: 30_000 });
      assert.match(await page.text('#toasts'), /A new version is available/);
      assert.equal(await swVersion(page), v1, 'the old version keeps running until the user agrees');

      await page.evaluate(() => {
        window.__oldDocument = true;
      });
      await page.click(tid('update-reload'));
      // The new worker takes over and the page reloads itself (a new document).
      await page.waitFor(() => !window.__oldDocument && document.body.classList.contains('core-ready') && navigator.serviceWorker.controller?.state === 'activated', [], {
        timeout: 30_000,
        message: 'the page to reload under the new version',
      });
      assert.equal(await swVersion(page), manifest.version);
      const names = await page.evaluate(() => caches.keys());
      assert.deepEqual(names.filter((n) => n.startsWith('convertible-app-')), [`convertible-app-${manifest.version}`], 'the old cache is deleted');
      const css = await page.evaluate(async () => (await fetch('css/app.css')).text());
      assert.ok(css.includes('deployment 2'), 'the new files are served');
    } finally {
      await own.close();
      await site.remove();
    }
  }, { timeout: 90_000 });

  e2e('a conversion while controlled by the service worker matches the online result', async ({ ctx }) => {
    const files = await tempDir();
    const page = await openApp(ctx, server());
    await waitForControl(page);
    await page.reload();
    await appReady(page);
    await chooseFiles(page, [await files.file('sw.txt', TEXT)]);
    await waitForReady(page, ['sw.txt']);
    await chooseTarget(page, 'sw.txt', 'hex');
    await convert(page, 'sw.txt');
    const d = await save(page, ctx, 'sw.txt', 'sw.hex');
    assert.equal(Buffer.from(d.bytes.toString('latin1').replace(/\s+/g, ''), 'hex').toString(), TEXT);
    await files.remove();
  });
});

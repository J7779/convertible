// The formats browser (against the live catalogue), settings persisted
// across reloads (theme, default quality, strip metadata), About and the
// open-source licences, and the shortcut deep links.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { appReady, chooseFiles, chooseTarget, openApp, tid, waitForReady } from './lib/app.mjs';
import { TEXT, tempDir } from './lib/fixtures.mjs';
import { DIST } from './lib/paths.mjs';
import { core, e2e, needsFormats, server, suite } from './lib/harness.mjs';

const FORMATS = 'dialog[data-testid="formats-dialog"][open]';
const SETTINGS = 'dialog[data-testid="settings-dialog"][open]';
const STORAGE_KEY = 'convertible.settings.v1';

const readSettings = (page) => page.evaluate((k) => JSON.parse(localStorage.getItem(k) || 'null'), STORAGE_KEY);

function rows(page) {
  return page.evaluate(() => [...document.querySelectorAll('dialog[data-testid="formats-dialog"] [data-testid^="format-"]')].map((r) => r.dataset.testid.slice('format-'.length)));
}

/** Background luminance of the page (0 = black, 1 = white). */
function luminance() {
  const [r, g, b] = getComputedStyle(document.body).backgroundColor.match(/[\d.]+/g).map(Number);
  return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
}

suite('formats browser and settings', () => {
  e2e('the formats browser lists the whole catalogue and searches it', async ({ ctx }) => {
    const formats = core.formats();
    const page = await openApp(ctx, server());
    await page.click(tid('open-formats'));
    await page.waitForSelector(FORMATS);
    const all = await rows(page);
    assert.deepEqual([...all].sort(), formats.map((f) => f.id).sort(), 'every format exactly once');
    const summary = await page.text(`${FORMATS} .sheet-sub`);
    assert.ok(summary.startsWith(`${formats.length} formats in `), summary);
    assert.ok(summary.includes(`${formats.filter((f) => f.readable).length} readable, ${formats.filter((f) => f.writable).length} writable`), summary);

    // Read/write pills follow the catalogue.
    for (const f of [formats[0], formats.at(-1)]) {
      const pills = await page.withElement(tid(`format-${f.id}`), (el) => [...el.querySelectorAll('.rw-pill')].map((p) => p.classList.contains('on')));
      assert.deepEqual(pills, [f.readable, f.writable], f.id);
    }

    // Search by extension: the format with that extension comes first.
    const probe = formats.find((f) => f.extensions?.length && f.extensions[0].length >= 3) ?? formats[0];
    await page.focus(tid('formats-search'));
    await page.type(`.${probe.extensions[0]}`);
    await page.waitFor((id) => document.querySelector('dialog[data-testid="formats-dialog"] [data-testid^="format-"]')?.dataset.testid === `format-${id}`, [probe.id], { message: `${probe.id} first` });
    const found = await rows(page);
    assert.ok(found.length < formats.length, 'the list is filtered');
    assert.match(await page.text(`${FORMATS} .sheet-sub`), /^\d+ match(es)?$/);

    // Escape clears the search field and the whole list comes back.
    await page.press('Escape');
    await page.waitFor((n) => document.querySelectorAll('dialog[data-testid="formats-dialog"] [data-testid^="format-"]').length === n, [formats.length], { message: 'the full list' });
    assert.ok(await page.evaluate(() => Boolean(document.querySelector('dialog[data-testid="formats-dialog"][open]'))), 'still open');
    await page.type('zzqqnotaformat');
    await page.waitForText('No matching formats.', { selector: FORMATS });
    assert.equal((await rows(page)).length, 0);

    await page.click(tid('formats-dialog-close'));
    await page.waitForGone(FORMATS);
    assert.equal(await page.evaluate(() => document.activeElement?.dataset.testid), 'open-formats', 'focus returns to the opener');
  });

  e2e('theme choice applies at once and persists across a reload', async ({ ctx }) => {
    const page = await openApp(ctx, server(), { colorScheme: 'dark' });
    const darkSystem = await page.evaluate(luminance);
    assert.ok(darkSystem < 0.25, `system theme follows the dark preference (${darkSystem})`);

    await page.click(tid('open-settings'));
    await page.waitForSelector(SETTINGS);
    assert.equal(await page.withElement(tid('theme-system'), (el) => el.checked), true);
    await page.click(tid('theme-light'));
    await page.waitFor(() => document.documentElement.dataset.theme === 'light');
    assert.ok((await page.evaluate(luminance)) > 0.75, 'light theme is light even though the system is dark');
    assert.equal((await readSettings(page)).theme, 'light');
    const themeColor = await page.evaluate(() => document.querySelector('meta[name="theme-color"]').content);

    await page.reload();
    await appReady(page);
    assert.equal(await page.evaluate(() => document.documentElement.dataset.theme), 'light');
    assert.ok((await page.evaluate(luminance)) > 0.75);
    assert.equal(await page.evaluate(() => document.querySelector('meta[name="theme-color"]').content), themeColor, 'the browser UI colour follows');
    await page.click(tid('open-settings'));
    await page.waitForSelector(SETTINGS);
    assert.equal(await page.withElement(tid('theme-light'), (el) => el.checked), true);

    // Back to "system": the attribute goes and the system preference rules again.
    await page.click(tid('theme-system'));
    await page.waitFor(() => !document.documentElement.hasAttribute('data-theme'));
    assert.ok((await page.evaluate(luminance)) < 0.25);
    await page.click(tid('settings-dialog-close'));
    await page.waitForGone(SETTINGS);
  });

  e2e('default quality and strip-metadata settings persist; only three preferences are stored', async ({ ctx }) => {
    const page = await openApp(ctx, server());
    await page.click(tid('open-settings'));
    await page.waitForSelector(SETTINGS);
    assert.equal(await page.withElement(tid('default-preset-balanced'), (el) => el.checked), true);
    assert.equal(await page.withElement(tid('strip-metadata'), (el) => el.checked), true);
    await page.click(tid('default-preset-small'));
    await page.waitFor(() => document.querySelector('[data-testid="default-preset-small"]')?.checked);
    await page.click(tid('strip-metadata'));
    await page.waitFor(() => document.querySelector('[data-testid="strip-metadata"]')?.checked === false);
    assert.deepEqual(await readSettings(page), { theme: 'system', preset: 'small', stripMetadata: false });

    await page.reload();
    await appReady(page);
    await page.click(tid('open-settings'));
    await page.waitForSelector(SETTINGS);
    assert.equal(await page.withElement(tid('default-preset-small'), (el) => el.checked), true);
    assert.equal(await page.withElement(tid('strip-metadata'), (el) => el.checked), false);
    // Privacy: nothing but the preferences is kept.
    const stored = await page.evaluate(() => ({ local: Object.keys(localStorage), session: Object.keys(sessionStorage) }));
    assert.deepEqual(stored.local, [STORAGE_KEY]);
    assert.deepEqual(stored.session.filter((k) => k !== 'convertible.intro'), []);
  });

  const managedCase = (() => {
    for (const source of ['txt', 'bin']) {
      for (const t of core?.targets(source) ?? []) {
        const schema = core.optionsSchema(source, t.id);
        if (schema.some((o) => o.key === 'strip_metadata') || schema.some((o) => o.key === 'preset')) return { source, target: t.id, schema };
      }
    }
    return null;
  })();

  e2e('settings apply to new cards (default quality preset, metadata note)', async ({ ctx }) => {
    const { source, target, schema } = managedCase;
    const files = await tempDir();
    const file = await files.file(`in.${source}`, source === 'txt' ? TEXT : Buffer.from([0, 1, 2, 3]));
    const page = await openApp(ctx, server());
    await page.evaluate((k) => localStorage.setItem(k, JSON.stringify({ theme: 'system', preset: 'small', stripMetadata: false })), STORAGE_KEY);
    await page.reload();
    await appReady(page);
    await chooseFiles(page, [file]);
    await waitForReady(page, [`in.${source}`]);
    await chooseTarget(page, `in.${source}`, target);
    if (schema.some((o) => o.key === 'preset')) {
      assert.equal(await page.withElement({ card: `in.${source}`, sel: tid('preset-small') }, (el) => el.checked), true);
    }
    if (schema.some((o) => o.key === 'strip_metadata')) await page.waitForText('Metadata is kept', { selector: '[data-testid="file-card"]' });
    await files.remove();
  }, { skip: needsFormats(managedCase, 'no text/binary target with a preset or metadata option') });

  e2e('About shows the versions; the licences open with their full text', async ({ ctx }) => {
    const buildInfo = await readFile(path.join(DIST, 'js', 'generated', 'build-info.js'), 'utf8');
    const appVersion = /APP_VERSION = "([^"]+)"/.exec(buildInfo)[1];
    const page = await openApp(ctx, server());
    await page.click(tid('open-settings'));
    await page.waitForSelector(SETTINGS);
    assert.equal(await page.text(tid('app-version')), appVersion);
    assert.equal(await page.text(tid('core-version')), core.version());
    assert.match(await page.text(SETTINGS), /Your files never leave this device/);

    await page.click(tid('open-licences'));
    await page.waitForSelector('dialog[data-testid="licences-dialog"][open]');
    const licences = await page.evaluate(() =>
      [...document.querySelectorAll('dialog[data-testid="licences-dialog"] details.licence')].map((d) => ({ title: d.querySelector('.licence-title').textContent, size: d.querySelector('pre').textContent.length })),
    );
    assert.equal(licences.length, 3);
    for (const l of licences) assert.ok(l.size > 500, `${l.title} has its full text`);
    await page.click(tid('licences-dialog-close'));
    await page.waitForGone('dialog[data-testid="licences-dialog"][open]');
  });

  e2e('shortcut links open the formats browser, settings and the file chooser', async ({ ctx }) => {
    const page = await openApp(ctx, server(), { query: '?view=formats' });
    await page.waitForSelector(FORMATS);
    assert.equal(await page.evaluate(() => location.search), '', 'the parameter is removed');
    const p2 = await openApp(ctx, server(), { query: '?view=settings' });
    await p2.waitForSelector(SETTINGS);
    const p3 = await openApp(ctx, server(), { query: '?action=open' });
    await p3.waitFor(() => document.activeElement?.dataset.testid === 'choose-files', [], { message: 'Choose files focused' });
  });
});

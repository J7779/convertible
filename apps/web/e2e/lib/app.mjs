// End-to-end tests - app-level steps shared by the test files: open the app,
// add files, pick targets, convert and download, read the queue.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { tid } from './browser.mjs';

export { tid };

/**
 * Opens the app in a new page of `ctx`.
 *   intro       play the startup animation (default: skipped, as on a second visit)
 *   fsAccess    keep the File System Access API (default: removed, so saving
 *               uses the app's download fallback, which headless Chrome can
 *               complete without a native dialog)
 *   width/height/mobile/colorScheme/reducedMotion  emulation, applied before load
 *   query       appended to the URL ("?view=settings")
 */
export async function openApp(ctx, server, { intro = false, fsAccess = false, query = '', waitReady = true, ...emulation } = {}) {
  const page = await ctx.newPage();
  await page.emulate(emulation);
  if (!intro) await page.addInitScript(`try { sessionStorage.setItem('convertible.intro', '1'); } catch {}`);
  if (!fsAccess) {
    await page.addInitScript(`
      for (const name of ['showSaveFilePicker', 'showDirectoryPicker', 'showOpenFilePicker']) {
        try { Object.defineProperty(window, name, { value: undefined, configurable: true, writable: true }); } catch {}
      }
    `);
  }
  await page.goto(server.url + query);
  if (waitReady) await appReady(page, { intro });
  return page;
}

/** Waits until the core's catalogue is loaded (and the intro is gone, unless `intro`). */
export async function appReady(page, { intro = false, timeout = 20_000 } = {}) {
  await page.waitFor(() => document.body.classList.contains('core-ready'), [], { timeout, message: 'the core to load (body.core-ready)' });
  if (!intro) await page.waitForGone('#intro', { timeout: 5_000 });
}

/** The queue as the user sees it: one entry per card. */
export function cards(page) {
  return page.evaluate(() =>
    [...document.querySelectorAll('[data-testid="file-card"]')].map((li) => {
      const badge = li.querySelector('.card-head .badge');
      const target = li.querySelector('[data-testid="target-picker"] .badge');
      return {
        key: li.dataset.key,
        name: li.querySelector('.file-name')?.textContent ?? '',
        state: li.dataset.state,
        badge: badge ? badge.textContent.trim() : null,
        unknown: badge ? badge.classList.contains('badge-unknown') : false,
        category: badge?.dataset.cat ?? null,
        target: target ? target.textContent.trim() : null,
        targetName: li.querySelector('[data-testid="target-picker"] .target-name')?.textContent ?? null,
      };
    }),
  );
}

export async function card(page, name) {
  const all = await cards(page);
  const found = all.find((c) => c.name === name);
  assert.ok(found, `no card named ${name}; cards: ${all.map((c) => c.name).join(', ')}`);
  return found;
}

/**
 * Resolves after the next animation frame: the app updates its toolbar
 * (counts, Convert all, Save all) in a requestAnimationFrame callback, and a
 * callback registered now runs after it.
 */
export function nextFrame(page) {
  return page.evaluate(() => new Promise((resolve) => requestAnimationFrame(() => resolve(true))));
}

/** Waits until the card `name` reaches `state` (or one of several states), and the toolbar caught up. */
export async function waitForCard(page, name, state, { timeout = 15_000 } = {}) {
  const states = Array.isArray(state) ? state : [state];
  const reached = await page.waitFor(
    (n, s) => {
      const li = [...document.querySelectorAll('[data-testid="file-card"]')].find((el) => el.querySelector('.file-name')?.textContent === n);
      return li && s.includes(li.dataset.state) ? li.dataset.state : false;
    },
    [name, states],
    { timeout, message: `card "${name}" to be ${states.join(' or ')}` },
  );
  await nextFrame(page);
  return reached;
}

/** Waits until every named card is ready (format detected, target chosen). */
export async function waitForReady(page, names, opts) {
  for (const n of names) await waitForCard(page, n, 'ready', opts);
}

/**
 * Adds files through the real "Choose files" button and the native file
 * chooser (intercepted and answered with `paths`).
 */
export async function chooseFiles(page, paths) {
  const queueVisible = await page.evaluate(() => !document.getElementById('queue').hidden);
  const chooser = await page.expectFileChooser(paths);
  await page.click(tid(queueVisible ? 'add-more' : 'choose-files'));
  await chooser.done;
}

/** Opens the target picker of a card and chooses `id` by searching for it. */
export async function chooseTarget(page, name, id) {
  await page.click({ card: name, sel: tid('target-picker') });
  await page.waitForSelector('dialog[data-testid="picker-dialog"][open]');
  await page.focus(tid('picker-search'));
  await page.type(id);
  await page.click({ sel: `dialog[open] ${tid(`target-option-${id}`)}` });
  await page.waitForGone('dialog[data-testid="picker-dialog"][open]');
  await page.waitFor(
    (n, label) => {
      const li = [...document.querySelectorAll('[data-testid="file-card"]')].find((el) => el.querySelector('.file-name')?.textContent === n);
      return li?.querySelector('[data-testid="target-picker"] .badge')?.textContent.trim() === label;
    },
    [name, id.toUpperCase()],
    { message: `card "${name}" to target ${id}` },
  );
}

/** Clicks Convert on a card and waits for the result. */
export async function convert(page, name, { timeout = 30_000 } = {}) {
  await page.click({ card: name, sel: tid('convert') });
  const state = await waitForCard(page, name, ['done', 'failed'], { timeout });
  if (state === 'failed') {
    const why = await page.text({ card: name, sel: tid('error') });
    assert.fail(`conversion of ${name} failed: ${why}`);
  }
}

/** Clicks Save on a finished card and returns the completed download {name, bytes, path}. */
export async function save(page, ctx, name, expectedName) {
  const after = ctx.downloadsSoFar();
  await page.click({ card: name, sel: tid('save') });
  return ctx.waitForDownload(expectedName, { after });
}

/** Sets a card option control (by its label text) through the DOM, firing change. */
export async function setOption(page, name, label, value) {
  await openAdvanced(page, name);
  await page.withElement({ card: name, sel: '.card-options' }, (root, lbl, val) => {
    const labelEl = [...root.querySelectorAll('label.field-label')].find((l) => l.textContent === lbl);
    if (!labelEl) throw new Error(`no option labelled ${lbl}`);
    const input = document.getElementById(labelEl.htmlFor);
    if (input.type === 'checkbox') input.checked = Boolean(val);
    else input.value = String(val);
    input.dispatchEvent(new Event('input', { bubbles: true }));
    input.dispatchEvent(new Event('change', { bubbles: true }));
  }, label, value);
}

/** Expands a card's options (<details data-testid=advanced>) with a click on its summary. */
export async function openAdvanced(page, name) {
  const open = await page.evaluate((n) => {
    const li = [...document.querySelectorAll('[data-testid="file-card"]')].find((el) => el.querySelector('.file-name')?.textContent === n);
    return li?.querySelector('[data-testid="advanced"]')?.open ?? null;
  }, name);
  if (open === false) {
    // Wait for the toggle event itself (the app records the open state there),
    // not just the attribute, so a re-render right after keeps it open.
    await page.withElement({ card: name, sel: tid('advanced') }, (el) => {
      window.__toggled = false;
      el.addEventListener('toggle', () => { window.__toggled = true; }, { once: true });
    });
    await page.click({ card: name, sel: `${tid('advanced')} > summary` });
    await page.waitFor(() => window.__toggled === true, [], { message: `the options of ${name} to open` });
  }
}

/** The labels of the option controls a card renders (preset excluded). */
export function optionLabels(page, name) {
  return page.withElement({ card: name, sel: '.card-options' }, (root) => [...root.querySelectorAll('label.field-label')].map((l) => l.textContent));
}

/** Expected download name: the input's stem plus the core's extension. */
export function outputName(inputName, extension) {
  const dot = inputName.lastIndexOf('.');
  return `${dot > 0 ? inputName.slice(0, dot) : inputName}.${extension}`;
}

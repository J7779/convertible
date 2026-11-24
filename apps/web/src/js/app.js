// Convertible web app - application controller: wires the core workers, the
// queue model and the views together.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { APP_VERSION, CORE_WASM_BYTES } from './generated/build-info.js';
import { CancelledError, CorePool } from './core/pool.js';
import { CoreLoadError, loadCoreModule } from './core/loader.js';
import { Status } from './core/binding.js';
import { Queue, State, isBusy, isFinished } from './model/queue.js';
import { buildControls, optionsFor } from './model/options.js';
import { compressTargets, defaultTarget, indexFormats } from './model/targets.js';
import { formatBytes, friendlyMessage, outputName, plural, statusTitle, formatLabel } from './model/format.js';
import { isTooLarge, tooLargeMessage } from './model/limits.js';
import { loadSettings, saveSettings } from './model/settings.js';
import { announce, clear, h, icon } from './ui/dom.js';
import { badge, renderCard, updateProgress } from './ui/cards.js';
import { openPicker } from './ui/picker.js';
import { createFormatsSheet, createLicencesSheet, createSettingsSheet } from './ui/sheets.js';
import { createProgressMark, playIntro, staticMark } from './ui/motion.js';
import { canOpen, canShareFile, filesFromClipboard, filesFromDataTransfer, hasFiles, isUserFile, openInTab, saveAll, saveFile, shareFile } from './platform/files.js';
import { consumeLaunchFiles, consumeSharedFiles, registerServiceWorker, setupInstall } from './platform/pwa.js';

const $ = (sel) => document.querySelector(sel);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let storage = null;
try {
  storage = window.localStorage;
} catch {
  storage = null;
}
let settings = loadSettings(storage);

const queue = new Queue();
const pool = new CorePool({ createWorker: () => new Worker(new URL('./core/worker.js', import.meta.url), { type: 'module', name: 'convertible-core' }) });

const catalogue = { formats: [], byId: new Map(), version: '', ready: false };
const targetsCache = new Map(); // source -> targets[] (resolved)
const targetsLoading = new Map(); // source -> Promise
const schemaCache = new Map(); // "src>dst" -> controls model
const schemaLoading = new Map();
const lastTargetBySource = new Map(); // remembered choice per source format (memory only)
const cards = new Map(); // key -> {li, record}
const advancedOpen = new Map();

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

function applyTheme() {
  const root = document.documentElement;
  if (settings.theme === 'system') root.removeAttribute('data-theme');
  else root.setAttribute('data-theme', settings.theme);
  const bg = getComputedStyle(root).getPropertyValue('--cv-color-background').trim() || '#0D0F16';
  for (const meta of document.querySelectorAll('meta[name="theme-color"]')) meta.setAttribute('content', bg);
}

function updateSettings(patch) {
  settings = { ...settings, ...patch };
  saveSettings(storage, settings);
  if ('theme' in patch) applyTheme();
  settingsSheet.refresh();
  // Preset and metadata defaults apply to cards that have not overridden them.
  for (const item of queue.items) if (!isBusy(item)) rerender(item.key);
}

matchMedia('(prefers-color-scheme: light)').addEventListener('change', applyTheme);

// ---------------------------------------------------------------------------
// Catalogue helpers
// ---------------------------------------------------------------------------

function targetsFor(source) {
  return targetsCache.get(source) ?? null;
}

function loadTargets(source) {
  if (targetsCache.has(source)) return Promise.resolve(targetsCache.get(source));
  if (!targetsLoading.has(source)) {
    targetsLoading.set(
      source,
      pool.targets(source).then((t) => {
        targetsCache.set(source, t);
        targetsLoading.delete(source);
        return t;
      }),
    );
  }
  return targetsLoading.get(source);
}

function schemaFor(source, target) {
  return schemaCache.get(`${source}>${target}`) ?? null;
}

function loadSchema(source, target) {
  const key = `${source}>${target}`;
  if (schemaCache.has(key)) return Promise.resolve(schemaCache.get(key));
  if (!schemaLoading.has(key)) {
    schemaLoading.set(
      key,
      pool
        .schema(source, target)
        .then((schema) => {
          const model = buildControls(schema);
          schemaCache.set(key, model);
          schemaLoading.delete(key);
          return model;
        })
        .catch(() => {
          schemaLoading.delete(key);
          return buildControls([]);
        }),
    );
  }
  return schemaLoading.get(key);
}

// ---------------------------------------------------------------------------
// Adding files
// ---------------------------------------------------------------------------

async function detectAndPrepare(item) {
  if (isTooLarge(item.size)) {
    queue.dispatch(item.key, { type: 'fail', error: { status: Status.LIMIT, title: statusTitle(Status.LIMIT), message: tooLargeMessage(item.size), permanent: true } });
    return;
  }
  let format = null;
  try {
    format = await pool.detect(item.file, item.name);
  } catch {
    format = null;
  }
  // Unrecognised files are still raw data: every universal target applies.
  const detected = Boolean(format);
  const source = format || 'bin';
  const info = catalogue.byId.get(source);
  const targets = await loadTargets(source).catch(() => []);
  const remembered = lastTargetBySource.get(source);
  const target = (remembered && targets.some((t) => t.id === remembered) ? remembered : defaultTarget(targets, info?.category)?.id) ?? null;
  if (target) await loadSchema(source, target);
  if (!queue.get(item.key)) return; // removed meanwhile
  queue.dispatch(item.key, { type: 'detected', format: source, detected, category: info?.category ?? 'other', target });
}

function addFiles(files) {
  const list = [...files].filter(Boolean);
  if (!list.length) return;
  const added = queue.add(list);
  announce(`${plural(added.length, 'file')} added.`);
  for (const item of added) detectAndPrepare(item);
  requestAnimationFrame(() => {
    const first = cards.get(added[0].key);
    if (first && added.length && !isInView(first.li)) first.li.scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'nearest' });
  });
}

function isInView(el) {
  const r = el.getBoundingClientRect();
  return r.top >= 0 && r.bottom <= window.innerHeight;
}

// ---------------------------------------------------------------------------
// Converting
// ---------------------------------------------------------------------------

async function convert(key) {
  const item = queue.get(key);
  if (!item || !item.target || !queue.dispatch(key, { type: 'enqueue' })) return;
  const model = await loadSchema(item.source, item.target);
  const current = queue.get(key);
  if (!current || current.state !== State.QUEUED) return;
  const options = optionsFor(model, current.options, settings);
  try {
    const reply = await pool.convert(
      key,
      { blob: current.file, name: current.name, from: current.source, to: current.target, options },
      {
        onStart: () => queue.dispatch(key, { type: 'start' }),
        onProgress: (fraction, stage) => queue.dispatch(key, { type: 'progress', fraction, stage }),
      },
    );
    if (reply.ok) {
      const mime = reply.mime || 'application/octet-stream';
      const blob = new Blob(reply.buffer ? [reply.buffer] : [], { type: mime });
      const name = outputName(current.name, reply.extension);
      queue.dispatch(key, { type: 'succeed', result: { blob, name, size: blob.size, mime, warnings: reply.warnings || [], format: reply.format } });
      announce(`${current.name} converted to ${name}, ${formatBytes(blob.size)}.`);
    } else if (reply.status === Status.CANCELLED) {
      queue.dispatch(key, { type: 'cancel' });
    } else {
      queue.dispatch(key, { type: 'fail', error: { status: reply.status, title: statusTitle(reply.status), message: friendlyMessage(reply.message) } });
      announce(`${current.name}: ${statusTitle(reply.status)}.`);
    }
  } catch (error) {
    if (error instanceof CancelledError) {
      queue.dispatch(key, { type: 'cancel' });
      return;
    }
    const status = error.status ?? Status.INTERNAL;
    queue.dispatch(key, { type: 'fail', error: { status, title: statusTitle(status), message: friendlyMessage(error.message) } });
    announce(`${current.name}: ${statusTitle(status)}.`);
  }
}

function convertAll() {
  const items = queue.convertible();
  for (const item of items) convert(item.key);
  if (items.length) announce(`Converting ${plural(items.length, 'file')}.`);
}

function cancel(key) {
  const item = queue.get(key);
  if (!item) return;
  if (!pool.cancel(key)) queue.dispatch(key, { type: 'cancel' });
  announce(`Cancelled ${item.name}.`);
}

function cancelAll() {
  pool.cancelAll();
  for (const item of queue.items) if (isBusy(item)) queue.dispatch(item.key, { type: 'cancel' });
  announce('All conversions cancelled.');
}

// ---------------------------------------------------------------------------
// Card actions
// ---------------------------------------------------------------------------

async function pickTarget(key, { compressOnly = false } = {}) {
  const item = queue.get(key);
  if (!item || !item.source) return;
  const all = (await loadTargets(item.source)) || [];
  const targets = compressOnly ? compressTargets(all) : all;
  if (!targets.length) return;
  const chosen = await openPicker({
    title: compressOnly ? `Compress ${item.name}` : `Convert ${item.sourceDetected ? formatLabel(item.source) : 'file'} to…`,
    subtitle: compressOnly ? 'Same format with smaller settings, or packed into an archive.' : item.name,
    targets,
    current: item.target,
    sourceCategory: item.category,
    formats: catalogue.byId,
    // Compress lists only the same format and archives: no "Suggested" duplicates.
    suggest: !compressOnly,
  });
  if (!chosen) {
    focusCard(key, 'target');
    return;
  }
  await loadSchema(item.source, chosen);
  lastTargetBySource.set(item.source, chosen);
  if (queue.get(key)?.target !== chosen) queue.dispatch(key, { type: 'setTarget', target: chosen });
  if (compressOnly) {
    const compressEntry = targets.find((t) => t.id === chosen && t.compress);
    if (compressEntry && schemaFor(item.source, chosen)?.preset) queue.dispatch(key, { type: 'setOptions', options: { preset: 'small' } });
  }
  focusCard(key, 'target');
}

async function pickTargetForGroup(source) {
  const targets = (await loadTargets(source)) || [];
  const items = queue.items.filter((i) => i.source === source);
  const info = catalogue.byId.get(source);
  const chosen = await openPicker({
    title: `Convert all ${formatLabel(source)} files to…`,
    subtitle: plural(items.length, 'file'),
    targets,
    current: items[0]?.target ?? null,
    sourceCategory: info?.category,
    formats: catalogue.byId,
  });
  if (!chosen) return;
  await loadSchema(source, chosen);
  lastTargetBySource.set(source, chosen);
  const n = queue.setTargetForSource(source, chosen);
  announce(`${plural(n, 'file')} set to ${formatLabel(chosen)}.`);
}

function focusCard(key, focusKey) {
  const card = cards.get(key);
  card?.li.querySelector(`[data-focus="${focusKey}"]`)?.focus();
}

async function save(key) {
  const item = queue.get(key);
  if (!item?.result) return;
  const outcome = await saveFile(item.result.blob, item.result.name, item.result.mime);
  if (outcome === 'saved') toast(`Saved ${item.result.name}`);
  else if (outcome === 'downloaded') toast(`Downloading ${item.result.name}`);
}

async function saveAllResults() {
  const done = queue.items.filter((i) => i.state === State.DONE && i.result);
  if (!done.length) return;
  if (done.length === 1) return save(done[0].key);
  // The core's C ABI converts one input at a time, so several results cannot
  // be packed into one archive through it yet; save them individually.
  const outcome = await saveAll(done.map((i) => ({ blob: i.result.blob, name: i.result.name })));
  if (outcome === 'saved') toast(`Saved ${plural(done.length, 'file')}`);
  else if (outcome === 'downloaded') toast(`Downloading ${plural(done.length, 'file')}`);
}

const actions = {
  pickTarget: (key) => pickTarget(key),
  compress: (key) => pickTarget(key, { compressOnly: true }),
  convert,
  cancel,
  remove: (key) => {
    pool.cancel(key);
    const item = queue.remove(key);
    if (item) announce(`Removed ${item.name}.`);
  },
  setOptions: (key, options) => queue.dispatch(key, { type: 'setOptions', options }),
  save,
  share: async (key) => {
    const r = queue.get(key)?.result;
    if (r) await shareFile(r.blob, r.name, r.mime);
  },
  open: (key) => {
    const r = queue.get(key)?.result;
    if (r) openInTab(r.blob, r.mime);
  },
  again: (key) => {
    queue.dispatch(key, { type: 'reset' });
    focusCard(key, 'target');
  },
};

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

const cardDeps = {
  get formats() {
    return catalogue.byId;
  },
  get settings() {
    return settings;
  },
  actions,
  targetsFor,
  schemaFor,
  openState: advancedOpen,
  canShare: (r) => canShareFile(r.blob, r.name, r.mime),
  canOpen: (r) => canOpen(r.mime),
};

function rerender(key) {
  const card = cards.get(key);
  const item = queue.get(key);
  if (card && item) renderCard(card.li, card.record, item, cardDeps);
}

function createCard(item) {
  const li = h('li', { class: 'card', 'data-testid': 'file-card', 'data-key': item.key });
  const record = { mark: null };
  cards.set(item.key, { li, record });
  renderCard(li, record, item, cardDeps);
  $('#cards').append(li);
  if (!matchMedia('(prefers-reduced-motion: reduce)').matches) {
    li.animate(
      [
        { opacity: 0, transform: 'translateY(12px) scale(0.98)' },
        { opacity: 1, transform: 'none' },
      ],
      { duration: 360, easing: 'cubic-bezier(0.16, 1, 0.3, 1)', delay: Math.min(6, $('#cards').children.length - 1) * 30, fill: 'backwards' },
    );
  }
}

function removeCard(key) {
  const card = cards.get(key);
  if (!card) return;
  cards.delete(key);
  const li = card.li;
  const hadFocus = li.contains(document.activeElement);
  const next = li.nextElementSibling || li.previousElementSibling;
  let removed = false;
  const done = () => {
    if (removed) return;
    removed = true;
    li.remove();
    if (hadFocus) (next?.querySelector('button') || $('#choose-files'))?.focus();
    renderChrome();
  };
  li.removeAttribute('data-testid'); // leaving: no longer one of the queue's cards
  li.inert = true;
  // Never wait on the animation alone: background tabs may not run it.
  setTimeout(done, 260);
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) done();
  else li.animate([{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'scale(0.96)' }], { duration: 180, easing: 'cubic-bezier(0.7, 0, 0.84, 0)' }).finished.then(done, done);
}

let chromeFrame = 0;
function renderChrome() {
  if (chromeFrame) return;
  chromeFrame = requestAnimationFrame(() => {
    chromeFrame = 0;
    const summary = queue.summary();
    const hasItems = summary.total > 0;
    document.body.classList.toggle('has-files', hasItems);
    $('#queue').hidden = !hasItems;
    $('#queue-count').textContent = hasItems ? plural(summary.total, 'file') : '';
    const convertible = queue.convertible().length;
    $('#convert-all').hidden = convertible === 0;
    $('#convert-all span').textContent = convertible > 1 ? `Convert ${convertible} files` : 'Convert';
    $('#cancel-all').hidden = summary.busy < 1;
    $('#save-all').hidden = summary.done < 2;
    $('#save-all span').textContent = `Save all (${summary.done})`;
    $('#clear-all').hidden = summary.busy > 0;
    const overall = $('#overall');
    overall.hidden = summary.busy === 0;
    overall.style.setProperty('--p', String(summary.overall));
    overall.setAttribute('aria-valuenow', String(Math.round(summary.overall * 100)));

    // "One control sets them all" for formats shared by several files.
    const bulk = $('#bulk');
    clear(bulk);
    for (const g of queue.sourceGroups()) {
      const items = g.keys.map((k) => queue.get(k));
      const targetIds = new Set(items.map((i) => i.target));
      const shared = targetIds.size === 1 ? [...targetIds][0] : null;
      const target = shared ? targetsFor(g.source)?.find((t) => t.id === shared) : null;
      const info = catalogue.byId.get(g.source);
      bulk.append(
        h(
          'div',
          { class: 'bulk-row', 'data-testid': `bulk-${g.source}` },
          h('span', { class: 'bulk-label' }, `All ${g.count}`, ' ', badge(g.source, info?.category, { detected: items[0].sourceDetected }), ' files to'),
          h(
            'button',
            { type: 'button', class: 'target-btn target-btn-compact', 'aria-haspopup': 'dialog', 'data-testid': 'bulk-target', onClick: () => pickTargetForGroup(g.source) },
            target ? badge(target.id, target.category, { shape: 'square' }) : h('span', { class: 'badge badge-pending' }, 'Mixed'),
            icon('chevron', { size: 16 }),
          ),
        ),
      );
    }
  });
}

queue.subscribe((change) => {
  if (change.type === 'add') for (const item of change.items) createCard(item);
  else if (change.type === 'remove') removeCard(change.item.key);
  else if (change.type === 'update') {
    const card = cards.get(change.item.key);
    if (!card) return;
    if (change.event.type === 'progress') updateProgress(card.li, card.record, change.item);
    else renderCard(card.li, card.record, change.item, cardDeps);
  }
  renderChrome();
});

// ---------------------------------------------------------------------------
// Toasts
// ---------------------------------------------------------------------------

function toast(text, { action = null, timeout = 4000 } = {}) {
  const region = $('#toasts');
  const el = h(
    'div',
    { class: 'toast', role: 'status' },
    h('span', {}, text),
    action ? h('button', { type: 'button', class: 'btn btn-ghost btn-small', 'data-testid': action.testid, onClick: () => { action.run(); el.remove(); } }, action.label) : null,
    h('button', { type: 'button', class: 'icon-btn icon-btn-small', 'aria-label': 'Dismiss', onClick: () => el.remove() }, icon('close', { size: 16 })),
  );
  region.append(el);
  if (timeout) setTimeout(() => el.remove(), timeout);
  return el;
}

// ---------------------------------------------------------------------------
// Sheets
// ---------------------------------------------------------------------------

const formatsSheet = createFormatsSheet();
const licencesSheet = createLicencesSheet();
const settingsSheet = createSettingsSheet({
  getSettings: () => settings,
  onChange: updateSettings,
  versions: () => ({ app: APP_VERSION, core: catalogue.version, formats: catalogue.formats.length }),
  openLicences: () => licencesSheet.open(),
});

// ---------------------------------------------------------------------------
// Input: picker, folder, drag & drop anywhere, paste, keyboard
// ---------------------------------------------------------------------------

function wireInput() {
  const fileInput = $('#file-input');
  const folderInput = $('#folder-input');
  const choose = () => fileInput.click();
  $('#choose-files').addEventListener('click', choose);
  $('#add-more').addEventListener('click', choose);
  $('#choose-folder').addEventListener('click', () => folderInput.click());
  for (const input of [fileInput, folderInput]) {
    input.addEventListener('change', () => {
      addFiles([...input.files].filter((f) => isUserFile(f) && !f.name.startsWith('.')));
      input.value = '';
    });
  }

  const overlay = $('#drop-overlay');
  let depth = 0;
  window.addEventListener('dragenter', (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    depth += 1;
    overlay.hidden = false;
    document.body.classList.add('is-dragging');
  });
  window.addEventListener('dragover', (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
  });
  window.addEventListener('dragleave', (e) => {
    if (!hasFiles(e)) return;
    depth = Math.max(0, depth - 1);
    if (depth === 0) {
      overlay.hidden = true;
      document.body.classList.remove('is-dragging');
    }
  });
  window.addEventListener('drop', async (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    depth = 0;
    overlay.hidden = true;
    document.body.classList.remove('is-dragging');
    addFiles(await filesFromDataTransfer(e.dataTransfer));
  });

  window.addEventListener('paste', (e) => {
    const t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
    if (document.querySelector('dialog[open]')) return;
    const files = filesFromClipboard(e);
    if (files.length) {
      e.preventDefault();
      addFiles(files);
    }
  });

  window.addEventListener('keydown', (e) => {
    if ((e.metaKey || e.ctrlKey) && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 'o') {
      e.preventDefault();
      choose();
    }
  });

  $('#convert-all').addEventListener('click', convertAll);
  $('#cancel-all').addEventListener('click', cancelAll);
  $('#save-all').addEventListener('click', saveAllResults);
  $('#clear-all').addEventListener('click', () => {
    const removed = queue.clear((i) => !isBusy(i));
    announce(`Cleared ${plural(removed.length, 'file')}.`);
    $('#choose-files').focus();
  });
  $('#open-formats').addEventListener('click', () => formatsSheet.open());
  $('#open-settings').addEventListener('click', () => settingsSheet.open());
}

// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

/** The catalogue ships as data (catalogue.json); the core is the fallback. */
async function fetchCatalogue() {
  try {
    const response = await fetch(new URL('../catalogue.json', import.meta.url));
    if (response.ok) {
      const data = await response.json();
      if (Array.isArray(data?.formats)) return { version: String(data.version ?? ''), formats: data.formats };
    }
  } catch {
    /* fall back to asking the core */
  }
  return pool.loadCatalogue();
}

async function loadCatalogue() {
  const { version, formats } = await fetchCatalogue();
  catalogue.formats = formats;
  catalogue.byId = indexFormats(formats);
  catalogue.version = version;
  catalogue.ready = true;
  formatsSheet.setFormats(formats);
  settingsSheet.refresh();
  $('#core-version').textContent = `core ${version}`;
  $('#format-count').textContent = String(formats.length);
  document.body.classList.add('catalogue-ready');
  markReady();
}

/** body.core-ready: the catalogue is shown and the converter runs. */
function markReady() {
  if (catalogue.ready && engine.state === 'ready') document.body.classList.add('core-ready');
}

// The core: one download + compile on the page, shared with every worker.
// The shell stays usable meanwhile (files can be added; they wait for it).
const engine = { state: 'idle', mark: null, shownAt: 0, timer: 0 };
const STATUS_DELAY_MS = 250; // a cached core starts without flashing the status

function renderEngineStatus({ loaded = 0, total = 0, error = null } = {}) {
  const box = $('#core-status');
  if (engine.state === 'ready') {
    if (engine.mark) engine.mark.complete();
    box.dataset.state = 'ready';
    // Leave the finished mark up briefly if it was shown at all.
    setTimeout(() => {
      box.hidden = true;
    }, box.hidden ? 0 : 600);
    return;
  }
  if (!engine.mark) engine.mark = createProgressMark({ size: 28 });
  box.dataset.state = engine.state;
  if (engine.state === 'failed') {
    engine.mark.reset();
    clear(box);
    box.append(
      engine.mark.el,
      h('span', { class: 'core-status-text' }, h('strong', {}, error?.kind === 'offline' ? 'You are offline. ' : 'The converter could not be loaded. '), error?.kind === 'unsupported' ? 'Please use a current version of Chrome, Edge, Firefox or Safari.' : 'Files you add will wait until it is ready.'),
      error?.kind === 'unsupported' ? null : h('button', { type: 'button', class: 'btn btn-secondary btn-small', 'data-testid': 'core-retry', onClick: () => startEngine() }, h('span', {}, 'Retry')),
    );
    box.hidden = false;
    return;
  }
  const fraction = total > 0 ? Math.min(1, loaded / total) : 0;
  if (fraction > 0) engine.mark.set(fraction);
  else engine.mark.spin();
  let text = box.querySelector('.core-status-text');
  if (!text || !box.contains(engine.mark.el)) {
    clear(box);
    text = h('span', { class: 'core-status-text' });
    box.append(engine.mark.el, text);
  }
  text.textContent =
    engine.state === 'compiling' || fraction >= 1
      ? 'Starting the converter…'
      : total > 0
        ? `Getting the converter ready: ${formatBytes(loaded)} of ${formatBytes(total)}`
        : 'Getting the converter ready…';
  box.setAttribute('aria-valuetext', text.textContent);
}

async function startEngine() {
  if (engine.state !== 'idle' && engine.state !== 'failed') return engine.state === 'ready';
  engine.state = 'loading';
  clearTimeout(engine.timer);
  let last = { loaded: 0, total: CORE_WASM_BYTES };
  engine.timer = setTimeout(() => {
    if (engine.state === 'ready') return;
    $('#core-status').hidden = false;
    renderEngineStatus(last);
  }, STATUS_DELAY_MS);
  let lastPaint = 0;
  try {
    const module = await loadCoreModule({
      url: new URL('../wasm/convertible.wasm', import.meta.url),
      expectedBytes: CORE_WASM_BYTES,
      onProgress: (loaded, total) => {
        last = { loaded, total };
        const now = performance.now();
        if (loaded >= total) engine.state = 'compiling';
        if (!$('#core-status').hidden && (now - lastPaint > 100 || loaded >= total)) {
          lastPaint = now;
          renderEngineStatus(last);
        }
      },
    });
    pool.provideModule(module);
    engine.state = 'ready';
    clearTimeout(engine.timer);
    renderEngineStatus();
    document.body.classList.add('engine-ready');
    markReady();
    return true;
  } catch (error) {
    engine.state = 'failed';
    clearTimeout(engine.timer);
    renderEngineStatus({ error: error instanceof CoreLoadError ? error : new CoreLoadError('compile', String(error)) });
    if (!(error instanceof CoreLoadError) || error.kind === 'unsupported' || error.kind === 'compile') return false;
    // Retry by itself as soon as the network comes back.
    window.addEventListener('online', () => startEngine(), { once: true });
    return false;
  }
}

function handleStartupParams() {
  const url = new URL(window.location.href);
  const view = url.searchParams.get('view');
  const action = url.searchParams.get('action');
  if (view || action) {
    url.searchParams.delete('view');
    url.searchParams.delete('action');
    history.replaceState(null, '', url.pathname + (url.search || '') + url.hash);
  }
  if (view === 'formats') formatsSheet.open();
  else if (view === 'settings') settingsSheet.open();
  else if (action === 'open') $('#choose-files').focus();
}

function setupBrand() {
  $('#brand-mark').replaceWith(staticMark({ size: 28, className: 'brand-mark' }));
  $('#hero-mark').replaceWith(staticMark({ size: 72, className: 'hero-mark' }));
  $('#app-version').textContent = `v${APP_VERSION}`;
}

async function start() {
  applyTheme();
  setupBrand();
  wireInput();
  renderChrome();

  // The intro plays over a usable app (once per session) and is skippable.
  const intro = $('#intro');
  let introSeen = false;
  try {
    introSeen = sessionStorage.getItem('convertible.intro') === '1';
    sessionStorage.setItem('convertible.intro', '1');
  } catch {
    introSeen = false;
  }
  const introDone = introSeen ? (intro.remove(), Promise.resolve()) : playIntro(intro);

  const engineStarted = startEngine();
  const ready = loadCatalogue().catch(() => {
    toast('The list of formats could not be loaded. Please reload the page.', { timeout: 0 });
    throw new Error('catalogue');
  });
  setupInstall($('#install'));
  consumeLaunchFiles(addFiles);
  ready.then(() => consumeSharedFiles(addFiles)).catch(() => {});
  await introDone;
  handleStartupParams();

  // The service worker precaches the whole app, the core included: start it
  // after the core has loaded (or failed), so the two never compete for the
  // network on a first visit, and the precache revalidates what the page has
  // just downloaded instead of fetching it again.
  await engineStarted;
  registerServiceWorker({
    onUpdate: (apply) => toast('A new version is available.', { action: { label: 'Reload', testid: 'update-reload', run: apply }, timeout: 0 }),
    onOfflineReady: () => toast('Ready to work offline.'),
  });
}

start();

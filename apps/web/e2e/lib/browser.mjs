// End-to-end tests - an in-house headless Chrome driver on top of cdp.mjs.
//
//   Browser   one Chrome process with a fresh temporary profile
//   Context   an isolated browser context (own storage, service workers,
//             caches and download directory): a "fresh profile" per test
//   Page      one tab, plus the dedicated workers it starts
//
// Every page, worker and service worker of a context reports into one
// Monitor: network requests, failed requests, console errors, uncaught
// exceptions and CSP violations, so tests can assert a clean run.
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { spawn } from 'node:child_process';
import { closeSync, existsSync, openSync, statSync } from 'node:fs';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { Connection } from './cdp.mjs';

// ---------------------------------------------------------------------------
// Finding Chrome
// ---------------------------------------------------------------------------

function isExecutable(file) {
  try {
    return statSync(file).isFile();
  } catch {
    return false;
  }
}

function onPath(names) {
  const dirs = (process.env.PATH || '').split(path.delimiter).filter(Boolean);
  const exts = process.platform === 'win32' ? ['.exe', ''] : [''];
  for (const name of names) for (const dir of dirs) for (const ext of exts) {
    const file = path.join(dir, name + ext);
    if (isExecutable(file)) return file;
  }
  return null;
}

/**
 * The Chrome/Chromium executable to drive: $CHROME_BIN, then the usual
 * install locations on macOS, Linux and Windows. Null when none is found.
 */
export function findChrome() {
  if (process.env.CHROME_BIN) return isExecutable(process.env.CHROME_BIN) ? process.env.CHROME_BIN : null;
  const home = os.homedir();
  const candidates = [];
  if (process.platform === 'darwin') {
    for (const root of ['/Applications', path.join(home, 'Applications')]) {
      candidates.push(
        path.join(root, 'Google Chrome.app/Contents/MacOS/Google Chrome'),
        path.join(root, 'Chromium.app/Contents/MacOS/Chromium'),
        path.join(root, 'Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing'),
        path.join(root, 'Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary'),
      );
    }
  } else if (process.platform === 'win32') {
    for (const root of [process.env.PROGRAMFILES, process.env['PROGRAMFILES(X86)'], process.env.LOCALAPPDATA].filter(Boolean)) {
      candidates.push(path.join(root, 'Google', 'Chrome', 'Application', 'chrome.exe'), path.join(root, 'Chromium', 'Application', 'chrome.exe'));
    }
  } else {
    const found = onPath(['google-chrome', 'google-chrome-stable', 'chromium', 'chromium-browser', 'chrome']);
    if (found) return found;
    candidates.push('/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser', '/snap/bin/chromium', '/opt/google/chrome/chrome');
  }
  return candidates.find(isExecutable) ?? null;
}

// ---------------------------------------------------------------------------
// Monitor
// ---------------------------------------------------------------------------

export class Monitor {
  constructor() {
    this.requests = []; // {url, method, from}
    this.failures = []; // {url, error, from}
    this.httpErrors = []; // {url, status, from}
    this.consoleErrors = []; // {text, from}
    this.exceptions = []; // {text, from}
    this.cspViolations = []; // {text, from}
  }

  reset() {
    for (const key of Object.keys(this)) this[key] = [];
  }

  /** Handles one CDP event from a session of kind `from` (page, worker, service_worker). */
  handle(method, params, from) {
    switch (method) {
      case 'Network.requestWillBeSent':
        this.requests.push({ url: params.request.url, method: params.request.method, from, id: params.requestId });
        break;
      case 'Network.responseReceived':
        if (params.response.status >= 400) this.httpErrors.push({ url: params.response.url, status: params.response.status, from });
        break;
      case 'Network.loadingFailed':
        this.failures.push({ id: params.requestId, error: params.errorText, canceled: Boolean(params.canceled), blocked: params.blockedReason ?? null, from });
        break;
      case 'Runtime.consoleAPICalled':
        if (params.type === 'error' || params.type === 'assert') {
          this.consoleErrors.push({ text: params.args.map((a) => a.value ?? a.description ?? '').join(' '), from });
        }
        break;
      case 'Runtime.exceptionThrown':
        this.exceptions.push({ text: params.exceptionDetails.exception?.description ?? params.exceptionDetails.text, from });
        break;
      case 'Log.entryAdded': {
        const { entry } = params;
        if (/Content.Security.Policy/i.test(entry.text)) this.cspViolations.push({ text: entry.text, from });
        else if (entry.level === 'error') this.consoleErrors.push({ text: `${entry.source}: ${entry.text}${entry.url ? ` (${entry.url})` : ''}`, from });
        break;
      }
      case 'Audits.issueAdded':
        if (params.issue.code === 'ContentSecurityPolicyIssue') {
          const d = params.issue.details.contentSecurityPolicyIssueDetails;
          this.cspViolations.push({ text: `${d?.violatedDirective}: ${d?.blockedURL ?? ''}`, from });
        }
        break;
      default:
        break;
    }
  }

  /** URL of a request id, for failure reports. */
  urlOf(id) {
    return this.requests.find((r) => r.id === id)?.url ?? '(unknown)';
  }
}

// ---------------------------------------------------------------------------
// Browser
// ---------------------------------------------------------------------------

const KEYS = {
  Enter: { code: 'Enter', keyCode: 13, text: '\r' },
  Tab: { code: 'Tab', keyCode: 9 },
  Escape: { code: 'Escape', keyCode: 27 },
  Space: { key: ' ', code: 'Space', keyCode: 32, text: ' ' },
  Backspace: { code: 'Backspace', keyCode: 8 },
  Delete: { code: 'Delete', keyCode: 46 },
  ArrowDown: { code: 'ArrowDown', keyCode: 40 },
  ArrowUp: { code: 'ArrowUp', keyCode: 38 },
  ArrowLeft: { code: 'ArrowLeft', keyCode: 37 },
  ArrowRight: { code: 'ArrowRight', keyCode: 39 },
  Home: { code: 'Home', keyCode: 36 },
  End: { code: 'End', keyCode: 35 },
  PageDown: { code: 'PageDown', keyCode: 34 },
  PageUp: { code: 'PageUp', keyCode: 33 },
};
const MODIFIERS = { Alt: 1, Control: 2, Meta: 4, Shift: 8 };

export class Browser {
  static async launch({ executable = findChrome(), args = [] } = {}) {
    if (!executable) throw new Error('no Chrome or Chromium found; set CHROME_BIN');
    const profile = await mkdtemp(path.join(os.tmpdir(), 'cvt-e2e-profile-'));
    const flags = [
      '--headless=new',
      `--user-data-dir=${profile}`,
      '--remote-debugging-port=0',
      '--no-first-run',
      '--no-default-browser-check',
      '--disable-background-networking',
      '--disable-component-update',
      '--disable-sync',
      '--disable-extensions',
      '--disable-default-apps',
      '--disable-domain-reliability',
      '--disable-client-side-phishing-detection',
      '--disable-breakpad',
      '--metrics-recording-only',
      '--password-store=basic',
      '--use-mock-keychain',
      '--mute-audio',
      '--hide-scrollbars',
      '--disable-features=Translate,OptimizationHints,MediaRouter,InterestFeedContentSuggestions',
      '--window-size=1280,900',
      // Containers often have a tiny /dev/shm; CI runners need no sandbox.
      ...(process.platform === 'linux' ? ['--disable-dev-shm-usage'] : []),
      ...(process.env.CI ? ['--no-sandbox'] : []),
      ...args,
      'about:blank',
    ];
    // Chrome's output goes to a log file in the profile (helper processes
    // inherit it, so a pipe would keep this process alive after Chrome quits);
    // the DevTools endpoint is read from the DevToolsActivePort file.
    const logFile = path.join(profile, 'chrome.log');
    const log = openSync(logFile, 'w');
    const child = spawn(executable, flags, { stdio: ['ignore', log, log] });
    closeSync(log);
    let exited = null;
    child.once('exit', (code, signal) => {
      exited = signal ?? code;
    });
    const portFile = path.join(profile, 'DevToolsActivePort');
    const deadline = Date.now() + 30_000;
    let wsUrl = null;
    while (!wsUrl) {
      if (exited !== null || Date.now() > deadline) {
        const tail = (await readFile(logFile, 'utf8').catch(() => '')).slice(-2000);
        throw new Error(`Chrome ${exited !== null ? `exited (${exited})` : 'did not start within 30 s'}:\n${tail}`);
      }
      const text = await readFile(portFile, 'utf8').catch(() => '');
      const [port, wsPath] = text.split('\n');
      if (port && wsPath) wsUrl = `ws://127.0.0.1:${port.trim()}${wsPath.trim()}`;
      else await delay(25);
    }
    const conn = await Connection.connect(wsUrl);
    const browser = new Browser(conn, child, profile);
    await browser.init();
    return browser;
  }

  constructor(conn, child, profile) {
    this.conn = conn;
    this.child = child;
    this.profile = profile;
    this.contexts = new Map(); // browserContextId -> Context
    this.sessions = new Map(); // sessionId -> {kind, context, page}
    this.downloads = new Map(); // guid -> {name, state, path}
  }

  async init() {
    const { product } = await this.conn.send('Browser.getVersion');
    this.version = product;
    this.conn.onEvent((m) => this.dispatch(m));
    // Pages auto-attach their workers and service workers (see Page.init),
    // paused until instrumented so their first requests are seen too.
  }

  dispatch({ method, params, sessionId }) {
    if (method === 'Target.attachedToTarget') {
      this.attached(params, sessionId).catch(() => {});
      return;
    }
    if (method === 'Target.detachedFromTarget') {
      this.sessions.delete(params.sessionId);
      return;
    }
    if (method === 'Browser.downloadWillBegin') {
      this.downloads.set(params.guid, { name: params.suggestedFilename, state: 'inProgress', url: params.url });
      return;
    }
    if (method === 'Browser.downloadProgress') {
      const d = this.downloads.get(params.guid);
      if (d) d.state = params.state;
      return;
    }
    const info = sessionId ? this.sessions.get(sessionId) : null;
    if (info) info.context.monitor.handle(method, params, info.kind);
  }

  async attached({ sessionId, targetInfo, waitingForDebugger }, parentSessionId) {
    if (this.sessions.has(sessionId)) return; // a session this driver opened itself
    const parent = parentSessionId ? this.sessions.get(parentSessionId) : null;
    const context = parent?.context ?? this.contexts.get(targetInfo.browserContextId);
    const run = () => (waitingForDebugger ? this.conn.send('Runtime.runIfWaitingForDebugger', {}, sessionId).catch(() => {}) : null);
    if (!context) {
      await run();
      return;
    }
    const kind = targetInfo.type;
    context.started[kind] = (context.started[kind] ?? 0) + 1;
    this.sessions.set(sessionId, { kind, context, page: parent?.page ?? null, targetId: targetInfo.targetId });
    if (parent?.page) parent.page.children.add(sessionId);
    const send = (m, p = {}) => this.conn.send(m, p, sessionId).catch(() => {});
    // Commands run in order on a session, so instrumenting first and then
    // resuming sees every request; nothing is awaited before resuming (a
    // paused service worker answers some commands only once it runs).
    const steps = [send('Network.enable'), send('Runtime.enable'), send('Log.enable')];
    if (context.offline) steps.push(send('Network.emulateNetworkConditions', OFFLINE));
    if (kind === 'worker') steps.push(send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }));
    steps.push(run());
    await Promise.all(steps);
  }

  async newContext() {
    const { browserContextId } = await this.conn.send('Target.createBrowserContext', { disposeOnDetach: true });
    const downloadDir = await mkdtemp(path.join(os.tmpdir(), 'cvt-e2e-downloads-'));
    await this.conn.send('Browser.setDownloadBehavior', { behavior: 'allowAndName', browserContextId, downloadPath: downloadDir, eventsEnabled: true });
    const context = new Context(this, browserContextId, downloadDir);
    this.contexts.set(browserContextId, context);
    return context;
  }

  async close() {
    for (const c of [...this.contexts.values()]) await c.close().catch(() => {});
    try {
      await this.conn.send('Browser.close');
    } catch {
      /* already gone */
    }
    this.conn.close();
    if (this.child.exitCode === null && this.child.signalCode === null) {
      await new Promise((resolve) => {
        const timer = setTimeout(() => {
          this.child.kill('SIGKILL');
          resolve();
        }, 5000);
        this.child.once('exit', () => {
          clearTimeout(timer);
          resolve();
        });
      });
    }
    await rm(this.profile, { recursive: true, force: true, maxRetries: 5 });
  }
}

const OFFLINE = { offline: true, latency: 0, downloadThroughput: -1, uploadThroughput: -1 };
const ONLINE = { offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1 };

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

export class Context {
  constructor(browser, id, downloadDir) {
    this.browser = browser;
    this.conn = browser.conn;
    this.id = id;
    this.downloadDir = downloadDir;
    this.monitor = new Monitor();
    this.pages = [];
    this.offline = false;
    /** Targets started so far by type ({worker: 3, service_worker: 1}). */
    this.started = {};
  }

  async newPage() {
    const { targetId } = await this.conn.send('Target.createTarget', { url: 'about:blank', browserContextId: this.id });
    const { sessionId } = await this.conn.send('Target.attachToTarget', { targetId, flatten: true });
    const page = new Page(this, targetId, sessionId);
    this.browser.sessions.set(sessionId, { kind: 'page', context: this, page, targetId });
    this.pages.push(page);
    await page.init();
    return page;
  }

  /** Every session of this context (pages, their workers, service workers). */
  sessionIds() {
    return [...this.browser.sessions.entries()].filter(([, s]) => s.context === this).map(([id]) => id);
  }

  /** Emulates a lost network in every page, worker and service worker of the context. */
  async setOffline(offline) {
    this.offline = offline;
    await Promise.all(this.sessionIds().map((id) => this.conn.send('Network.emulateNetworkConditions', offline ? OFFLINE : ONLINE, id).catch(() => {})));
  }

  /**
   * Waits until a download named `name` has completed in this context and
   * returns {name, path, bytes}. Downloads are matched by suggested name.
   */
  async waitForDownload(name, { timeout = 15_000, after = new Set() } = {}) {
    const deadline = Date.now() + timeout;
    for (;;) {
      for (const [guid, d] of this.browser.downloads) {
        if (after.has(guid) || d.name !== name) continue;
        const file = path.join(this.downloadDir, guid);
        if (d.state === 'completed' && existsSync(file)) {
          after.add(guid);
          return { name: d.name, guid, path: file, bytes: await readFile(file) };
        }
        if (d.state === 'canceled') throw new Error(`download ${name} was cancelled`);
      }
      if (Date.now() > deadline) {
        const seen = [...this.browser.downloads.values()].map((d) => `${d.name} (${d.state})`).join(', ') || 'none';
        throw new Error(`timed out waiting for the download ${name}; downloads seen: ${seen}`);
      }
      await delay(25);
    }
  }

  /** Guids of every download seen so far (to wait only for newer ones). */
  downloadsSoFar() {
    return new Set(this.browser.downloads.keys());
  }

  async close() {
    this.browser.contexts.delete(this.id);
    for (const [id, s] of [...this.browser.sessions]) if (s.context === this) this.browser.sessions.delete(id);
    await this.conn.send('Target.disposeBrowserContext', { browserContextId: this.id }).catch(() => {});
    await rm(this.downloadDir, { recursive: true, force: true, maxRetries: 5 });
  }
}

/** Polling interval of the condition waits (not a sleep: the condition decides). */
const POLL_MS = 25;
const delay = (ms) => new Promise((r) => setTimeout(r, ms));

function isContextLoss(error) {
  return /Execution context was destroyed|Cannot find context|Inspected target navigated|context with specified id|Target closed|uniqueContextId not found/i.test(String(error?.message));
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

/** In-page locator: a CSS selector, optionally inside the file card whose name is `card`. */
const LOCATE = `(spec) => {
  let root = document;
  if (spec.card != null) {
    root = [...document.querySelectorAll('[data-testid="file-card"]')].find((li) => li.querySelector('.file-name')?.textContent === spec.card);
    if (!root) return null;
  }
  if (spec.within) {
    root = root.querySelector(spec.within);
    if (!root) return null;
  }
  const all = [...root.querySelectorAll(spec.sel)];
  return all[spec.index ?? 0] ?? null;
}`;

function spec(target) {
  if (typeof target === 'string') return { sel: target };
  return target;
}

function describe(target) {
  const s = spec(target);
  return `${s.sel}${s.card != null ? ` in card "${s.card}"` : ''}${s.within ? ` within ${s.within}` : ''}${s.index ? ` #${s.index}` : ''}`;
}

/** `[data-testid="x"]` */
export const tid = (id) => `[data-testid="${id}"]`;

export class Page {
  constructor(context, targetId, sessionId) {
    this.context = context;
    this.conn = context.conn;
    this.targetId = targetId;
    this.sessionId = sessionId;
    this.children = new Set();
    this.fileChooserFiles = null;
  }

  send(method, params = {}) {
    return this.conn.send(method, params, this.sessionId);
  }

  async init() {
    await Promise.all([
      this.send('Page.enable'),
      this.send('Runtime.enable'),
      this.send('Network.enable'),
      this.send('Log.enable'),
      this.send('Audits.enable').catch(() => {}),
      this.send('DOM.enable'),
    ]);
    await this.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: true, flatten: true });
    // Record CSP violations the page itself sees, too (belt and braces with Log).
    await this.addInitScript(`
      window.__cspViolations = [];
      document.addEventListener('securitypolicyviolation', (e) => window.__cspViolations.push(e.violatedDirective + ' ' + e.blockedURI));
    `);
  }

  /** Runs `source` in every new document of this page, before its scripts. */
  async addInitScript(source) {
    await this.send('Page.addScriptToEvaluateOnNewDocument', { source });
  }

  // -- navigation -------------------------------------------------------------

  async goto(url, { timeout = 30_000 } = {}) {
    const loaded = this.conn.waitForEvent('Page.loadEventFired', { sessionId: this.sessionId, timeout });
    const { errorText } = await this.send('Page.navigate', { url });
    if (errorText) {
      loaded.catch(() => {});
      throw new Error(`navigation to ${url} failed: ${errorText}`);
    }
    await loaded;
  }

  async reload({ timeout = 30_000 } = {}) {
    const loaded = this.conn.waitForEvent('Page.loadEventFired', { sessionId: this.sessionId, timeout });
    await this.send('Page.reload', { ignoreCache: false });
    await loaded;
  }

  async close() {
    await this.conn.send('Target.closeTarget', { targetId: this.targetId }).catch(() => {});
  }

  // -- script -----------------------------------------------------------------

  /** Evaluates `fn(...args)` in the page (functions are serialised) and returns the JSON value. */
  async evaluate(fn, ...args) {
    // Page functions may call `locate(spec)` (see LOCATE); no eval in the
    // page, which the app's CSP forbids.
    const expression = typeof fn === 'function' ? `((locate) => (${fn})(...${JSON.stringify(args)}))(${LOCATE})` : String(fn);
    const { result, exceptionDetails } = await this.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true, userGesture: false });
    if (exceptionDetails) {
      throw new Error(`page script failed: ${exceptionDetails.exception?.description ?? exceptionDetails.text}`);
    }
    return result.value;
  }

  /**
   * Polls `fn(...args)` in the page until it returns a truthy value, which is
   * returned. Survives navigations. Throws `message` after `timeout` ms.
   */
  async waitFor(fn, args = [], { timeout = 10_000, message = null } = {}) {
    const deadline = Date.now() + timeout;
    let last = null;
    for (;;) {
      try {
        const value = await this.evaluate(fn, ...args);
        if (value) return value;
        last = value;
      } catch (error) {
        if (!isContextLoss(error)) throw error;
      }
      if (Date.now() > deadline) {
        throw new Error(`timed out after ${timeout} ms: ${message ?? String(fn).slice(0, 200)} (last value: ${JSON.stringify(last)})`);
      }
      await delay(POLL_MS);
    }
  }

  /** Waits for an element (optionally visible) and returns true. */
  waitForSelector(target, { visible = true, timeout = 10_000 } = {}) {
    return this.waitFor(
      (s, vis) => {
        const el = locate(s);
        if (!el) return false;
        if (!vis) return true;
        const r = el.getBoundingClientRect();
        const cs = getComputedStyle(el);
        return r.width > 0 && r.height > 0 && cs.visibility !== 'hidden' && !el.closest('[hidden]');
      },
      [spec(target), visible],
      { timeout, message: `element ${describe(target)} to be ${visible ? 'visible' : 'present'}` },
    );
  }

  waitForGone(target, { timeout = 10_000 } = {}) {
    return this.waitFor((s) => !locate(s), [spec(target)], { timeout, message: `element ${describe(target)} to disappear` });
  }

  /** Waits until the page's visible text contains `text` (within `selector`). */
  waitForText(text, { selector = 'body', timeout = 10_000 } = {}) {
    return this.waitFor((sel, t) => [...document.querySelectorAll(sel)].some((el) => el.innerText.includes(t)), [selector, text], {
      timeout,
      message: `text "${text}" in ${selector}`,
    });
  }

  /** Runs `fn(element, ...args)` on a located element. */
  async withElement(target, fn, ...args) {
    const s = JSON.stringify(spec(target));
    return this.evaluate(`((locate) => {
      const el = locate(${s});
      if (!el) throw new Error('no element ' + ${JSON.stringify(describe(target))});
      return (${fn})(el, ...${JSON.stringify(args)});
    })(${LOCATE})`);
  }

  count(selector) {
    return this.evaluate((sel) => document.querySelectorAll(sel).length, selector);
  }

  text(target) {
    return this.withElement(target, (el) => el.innerText.replace(/\s+/g, ' ').trim());
  }

  // -- input ------------------------------------------------------------------

  /**
   * Clicks an element like a user: scrolls it into view, waits until it is
   * enabled and is what a click at its centre would hit, then presses and
   * releases the mouse there.
   */
  async click(target, { timeout = 10_000 } = {}) {
    const point = await this.waitFor(
      (s) => {
        const el = locate(s);
        if (!el || el.disabled || el.closest('[inert]')) return false;
        // Visually hidden inputs (radios in segmented controls) are clicked through their label.
        const own = el.getBoundingClientRect();
        const box = (own.width <= 2 || own.height <= 2) && el.labels && el.labels[0] ? el.labels[0] : el;
        box.scrollIntoView({ block: 'center', inline: 'center', behavior: 'instant' });
        const r = box.getBoundingClientRect();
        if (r.width <= 0 || r.height <= 0) return false;
        const x = r.left + r.width / 2;
        const y = r.top + r.height / 2;
        const hit = document.elementFromPoint(x, y);
        if (!hit || !(el === hit || el.contains(hit) || (el.labels && [...el.labels].some((l) => l.contains(hit))))) return false;
        // Not moving: no running animation or transition on it or an ancestor
        // (a sheet sliding in), and the same position on the next frame.
        const moving = document.getAnimations().some((a) => a.playState === 'running' && a.effect && a.effect.target instanceof Element && a.effect.target.contains(el) && Number.isFinite(a.effect.getComputedTiming().endTime));
        if (moving) return false;
        // Web fonts still loading would reflow the page under the pointer.
        if (document.fonts && document.fonts.status !== 'loaded') return false;
        // Two frames without movement (the app updates its toolbar in a frame callback).
        return new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(() => {
          const r2 = box.getBoundingClientRect();
          if (Math.abs(r2.left - r.left) >= 0.5 || Math.abs(r2.top - r.top) >= 0.5) return resolve(false);
          const again = document.elementFromPoint(x, y);
          if (!again || !(box === again || box.contains(again))) return resolve(false);
          // Probe: record where the click event really lands.
          for (const old of document.querySelectorAll('[data-e2e-click]')) old.removeAttribute('data-e2e-click');
          el.setAttribute('data-e2e-click', '');
          box.setAttribute('data-e2e-click', '');
          window.__e2eClick = { landed: null, detached: false };
          window.addEventListener('click', (e) => {
            const t = e.target instanceof Element ? e.target : null;
            window.__e2eClick.landed = t ? (t.closest('[data-e2e-click]') ? 'target' : `${t.tagName.toLowerCase()}${t.dataset?.testid ? `[${t.dataset.testid}]` : ''}.${String(t.className?.baseVal ?? t.className).split(' ')[0]}`) : 'nothing';
          }, { capture: true, once: true });
          resolve({ x, y });
        })));
      },
      [spec(target)],
      { timeout, message: `element ${describe(target)} to be clickable` },
    );
    await this.mouseClick(point.x, point.y);
    // The click must have reached the element (or, if the app re-rendered it
    // in between, an element in the same place). A miss is reported, not retried.
    const probe = await this.evaluate(() => {
      if (!window.__e2eClick) return { navigated: true }; // a new document: the click navigated
      const p = window.__e2eClick;
      const marked = document.querySelector('[data-e2e-click]');
      for (const el of document.querySelectorAll('[data-e2e-click]')) el.removeAttribute('data-e2e-click');
      return { landed: p.landed ?? null, rerendered: !marked };
    }).catch(() => ({ navigated: true })); // navigating right now
    if (!probe.navigated && probe.landed !== 'target' && !(probe.rerendered && probe.landed !== null)) {
      throw new Error(`click on ${describe(target)} landed on ${probe.landed ?? 'nothing'}`);
    }
  }

  async mouseClick(x, y) {
    await this.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y });
    await this.send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1 });
    await this.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount: 1 });
  }

  /** Types text into the focused element (as an IME commit: fires input events). */
  async type(text) {
    await this.send('Input.insertText', { text });
  }

  /** Presses a key ("Enter", "Tab", "Shift+Tab", "a"...). */
  async press(combo) {
    const parts = combo.split('+');
    const name = parts.pop();
    const modifiers = parts.reduce((m, p) => m | (MODIFIERS[p] ?? 0), 0);
    const def = KEYS[name] ?? { key: name, code: `Key${name.toUpperCase()}`, keyCode: name.toUpperCase().charCodeAt(0), text: name };
    const key = def.key ?? name;
    const base = { key, code: def.code, windowsVirtualKeyCode: def.keyCode, nativeVirtualKeyCode: def.keyCode, modifiers };
    const text = modifiers & (MODIFIERS.Control | MODIFIERS.Meta | MODIFIERS.Alt) ? undefined : def.text;
    await this.send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', ...base, text, unmodifiedText: text });
    await this.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
  }

  async focus(target) {
    await this.withElement(target, (el) => el.focus());
  }

  /** Sets the files of an <input type=file> (fires input and change). */
  async setFiles(target, files) {
    const objectId = await this.objectId(target);
    await this.send('DOM.setFileInputFiles', { files, objectId });
  }

  async objectId(target) {
    const { result, exceptionDetails } = await this.send('Runtime.evaluate', { expression: `(${LOCATE})(${JSON.stringify(spec(target))})` });
    if (exceptionDetails || !result.objectId) throw new Error(`no element ${describe(target)}`);
    return result.objectId;
  }

  /**
   * Intercepts the next native file chooser (opened by a click or a key
   * press after this call) and answers it with `files`. Call before the
   * action; await `.done` after it.
   */
  async expectFileChooser(files, { timeout = 10_000 } = {}) {
    await this.send('Page.setInterceptFileChooserDialog', { enabled: true });
    const opened = this.conn.waitForEvent('Page.fileChooserOpened', { sessionId: this.sessionId, timeout });
    return {
      done: (async () => {
        const params = await opened;
        await this.send('DOM.setFileInputFiles', { files, backendNodeId: params.backendNodeId });
        await this.send('Page.setInterceptFileChooserDialog', { enabled: false });
      })(),
    };
  }

  /**
   * Drags files from "the desktop" onto an element: native dragenter,
   * dragover and drop events carrying real files.
   */
  async dropFiles(target, files) {
    const { x, y } = await this.withElement(target, (el) => {
      el.scrollIntoView({ block: 'center', behavior: 'instant' });
      const r = el.getBoundingClientRect();
      return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
    });
    const data = { items: [], files, dragOperationsMask: 1 };
    await this.send('Input.dispatchDragEvent', { type: 'dragEnter', x, y, data });
    await this.send('Input.dispatchDragEvent', { type: 'dragOver', x, y, data });
    return {
      drop: () => this.send('Input.dispatchDragEvent', { type: 'drop', x, y, data }),
      cancel: () => this.send('Input.dispatchDragEvent', { type: 'dragCancel', x, y, data }),
    };
  }

  // -- emulation --------------------------------------------------------------

  async emulate({ width = null, height = null, deviceScaleFactor = 1, mobile = false, touch = mobile, colorScheme = null, reducedMotion = null } = {}) {
    if (width) {
      await this.send('Emulation.setDeviceMetricsOverride', { width, height: height ?? 800, deviceScaleFactor, mobile });
      await this.send('Emulation.setTouchEmulationEnabled', touch ? { enabled: true, maxTouchPoints: 5 } : { enabled: false });
    }
    const features = [];
    if (colorScheme) features.push({ name: 'prefers-color-scheme', value: colorScheme });
    if (reducedMotion) features.push({ name: 'prefers-reduced-motion', value: reducedMotion });
    if (features.length) await this.send('Emulation.setEmulatedMedia', { features });
  }

  // -- diagnostics ------------------------------------------------------------

  async screenshot(file) {
    const { data } = await this.send('Page.captureScreenshot', { format: 'png' });
    await mkdir(path.dirname(file), { recursive: true });
    await writeFile(file, Buffer.from(data, 'base64'));
  }
}

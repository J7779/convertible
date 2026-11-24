// End-to-end tests - suite and test wrappers on node:test.
//
//   suite(name, body)      one Chrome and one app server per test file
//   e2e(name, fn, opts)    one fresh browser context ("profile") per test;
//                          afterwards asserts a clean run (no console
//                          errors, exceptions, CSP violations, failed or
//                          cross-origin requests); on failure saves a
//                          screenshot of every page and the monitor log
//                          into e2e/artifacts/ (git-ignored)
//
// No retries: a flaky test is a bug.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { after, before, describe, test } from 'node:test';
import { Browser, findChrome } from './browser.mjs';
import { startServer } from './server.mjs';
import { ARTIFACTS, DIST, WEB_DIR } from './paths.mjs';

export const CHROME = findChrome();
export const REQUIRE_BROWSER = process.env.E2E_REQUIRE_BROWSER === '1';
export const NO_BROWSER_NOTE = 'skipped: no Chrome/Chromium found (set CHROME_BIN, or E2E_REQUIRE_BROWSER=1 to fail instead)';

const HAS_DIST = existsSync(path.join(DIST, 'wasm', 'convertible.wasm'));

/**
 * The real core, loaded in Node from the same build the app serves: tests
 * ask it for the catalogue instead of hard-coding format lists.
 */
export const core = HAS_DIST
  ? await (async () => {
      const { loadCore } = await import(pathToFileURL(path.join(WEB_DIR, 'tests', 'helpers.mjs')).href);
      return loadCore();
    })()
  : null;

/** The app's own pure modules (labels, size formatting), imported in Node. */
export const appModule = (rel) => import(pathToFileURL(path.join(WEB_DIR, 'src', 'js', rel)).href);

const shared = { browser: null, server: null };

/** The running app server of the current file: {url, origin, port}. */
export function server() {
  return shared.server;
}

/** Declares a test file's suite: launches Chrome and the server around it. */
export function suite(name, body) {
  const skip = !CHROME && !REQUIRE_BROWSER ? NO_BROWSER_NOTE : false;
  describe(name, { skip }, () => {
    before(async () => {
      if (!CHROME) throw new Error('E2E_REQUIRE_BROWSER=1 but no Chrome/Chromium was found (set CHROME_BIN)');
      if (!HAS_DIST) throw new Error(`no build in ${DIST}; run apps/web/e2e.sh (it builds first) or apps/web/build.sh`);
      shared.browser = await Browser.launch({ executable: CHROME });
      shared.server = await startServer(DIST);
    });
    after(async () => {
      await shared.browser?.close();
      await shared.server?.close();
    });
    body();
  });
}

function slug(text) {
  return text.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 80);
}

/** True for URLs that belong to the app's own origin (including its blob: URLs). */
export function sameOrigin(url, origin) {
  if (url.startsWith('data:')) return true;
  if (url.startsWith('blob:')) return url.startsWith(`blob:${origin}/`);
  try {
    return new URL(url).origin === origin;
  } catch {
    return false;
  }
}

/**
 * Asserts the context ran cleanly. `allow` may relax specific checks:
 *   allow.failure(f)  -> true to accept a failed request {url, error}
 *   allow.console(c)  -> true to accept a console error {text}
 */
export async function assertClean(ctx, origin, allow = {}) {
  const m = ctx.monitor;
  const foreign = m.requests.filter((r) => !sameOrigin(r.url, origin));
  assert.deepEqual(foreign.map((r) => `${r.from} ${r.url}`), [], 'every request must be same-origin');
  const failures = m.failures
    .map((f) => ({ ...f, url: m.urlOf(f.id) }))
    // A download of a blob: URL ends the page's request with ERR_ABORTED by design.
    .filter((f) => !(f.url.startsWith('blob:') && f.error === 'net::ERR_ABORTED'))
    .filter((f) => !(allow.failure && allow.failure(f)));
  assert.deepEqual(failures.map((f) => `${f.from} ${f.url}: ${f.error}`), [], 'no request may fail');
  assert.deepEqual(m.httpErrors.map((e) => `${e.from} ${e.url}: HTTP ${e.status}`), [], 'no HTTP errors');
  const errors = m.consoleErrors.filter((c) => !(allow.console && allow.console(c)));
  assert.deepEqual(errors.map((c) => `${c.from}: ${c.text}`), [], 'no console errors');
  assert.deepEqual(m.exceptions.map((e) => `${e.from}: ${e.text}`), [], 'no uncaught exceptions');
  assert.deepEqual(m.cspViolations.map((v) => `${v.from}: ${v.text}`), [], 'no CSP violations (DevTools)');
  for (const page of ctx.pages) {
    let seen = [];
    try {
      seen = (await page.evaluate(() => window.__cspViolations || [])) ?? [];
    } catch {
      seen = []; // page closed
    }
    assert.deepEqual(seen, [], 'no CSP violations (securitypolicyviolation events)');
  }
}

async function saveArtifacts(ctx, name, error) {
  try {
    await mkdir(ARTIFACTS, { recursive: true });
    const base = path.join(ARTIFACTS, `${slug(path.basename(process.argv[1] || 'e2e'))}--${slug(name)}`);
    let i = 0;
    for (const page of ctx.pages) {
      i += 1;
      await page.screenshot(`${base}--page${i}.png`).catch(() => {});
    }
    const m = ctx.monitor;
    const log = [
      `error: ${error?.stack ?? error}`,
      '',
      'requests:',
      ...m.requests.map((r) => `  ${r.from} ${r.method} ${r.url}`),
      'failures:',
      ...m.failures.map((f) => `  ${f.from} ${m.urlOf(f.id)} ${f.error}`),
      'console errors:',
      ...m.consoleErrors.map((c) => `  ${c.from} ${c.text}`),
      'exceptions:',
      ...m.exceptions.map((c) => `  ${c.from} ${c.text}`),
      'csp:',
      ...m.cspViolations.map((c) => `  ${c.from} ${c.text}`),
    ].join('\n');
    await writeFile(`${base}.log`, `${log}\n`);
  } catch {
    /* diagnostics must never mask the real failure */
  }
}

/**
 * One end-to-end test in a fresh browser context.
 * fn({ctx, server, t}) ; opts: {timeout, skip, allow, clean}
 */
export function e2e(name, fn, { timeout = 60_000, skip = false, allow = {}, clean = true } = {}) {
  test(name, { timeout, skip }, async (t) => {
    const ctx = await shared.browser.newContext();
    try {
      await fn({ ctx, server: shared.server, t });
      // A test that serves the app itself sets ctx.origin to that server's origin.
      if (clean) await assertClean(ctx, ctx.origin ?? shared.server.origin, allow);
    } catch (error) {
      await saveArtifacts(ctx, name, error);
      throw error;
    } finally {
      await ctx.close();
    }
  });
}

/** A visible reason for catalogue-conditional tests ("skipped: format not in catalogue ..."). */
export function needsFormats(ok, what) {
  return ok ? false : `skipped: format not in catalogue (${what})`;
}

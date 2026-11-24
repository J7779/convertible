// First load: the startup animation over a usable app, skipping it, the
// reduced-motion static mark, once per session, and a clean load.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { appReady, cards, chooseFiles, openApp, tid, waitForReady } from './lib/app.mjs';
import { TEXT, tempDir } from './lib/fixtures.mjs';
import { core, e2e, server, suite } from './lib/harness.mjs';

/** Records, in page time, when the intro mark appeared and when the intro left. */
const INTRO_PROBE = `
  window.__intro = { markAt: null, goneAt: null };
  new MutationObserver((records) => {
    for (const r of records) {
      for (const n of r.addedNodes) if (n.classList && n.classList.contains('intro-mark') && window.__intro.markAt === null) window.__intro.markAt = performance.now();
      for (const n of r.removedNodes) if (n.id === 'intro' && window.__intro.goneAt === null) window.__intro.goneAt = performance.now();
    }
  }).observe(document, { childList: true, subtree: true });
`;

/** Running animations inside the intro: [{target, currentTime, duration}]. */
function introAnimations() {
  const intro = document.getElementById('intro');
  if (!intro) return null;
  return document
    .getAnimations()
    .filter((a) => a.playState === 'running' && a.effect && intro.contains(a.effect.target))
    .map((a) => ({ target: a.effect.target.getAttribute('class') || a.effect.target.tagName, currentTime: a.currentTime, duration: a.effect.getTiming().duration }));
}

async function openWithIntro(ctx, opts = {}) {
  const page = await ctx.newPage();
  await page.emulate(opts);
  await page.addInitScript(INTRO_PROBE);
  await page.goto(server().url);
  return page;
}

suite('startup', () => {
  e2e('first load plays the brand intro over the app; a click skips it and the app is usable', async ({ ctx }) => {
    const files = await tempDir();
    const page = await openWithIntro(ctx);
    // The storyboard animates several layers of the mark.
    const running = await page.waitFor(introAnimations, [], { message: 'the intro animation to run' });
    const layers = new Set(running.map((a) => a.target));
    assert.ok(layers.size >= 2, `several layers animate: ${[...layers].join(', ')}`);
    const duration = Math.max(...running.map((a) => a.duration));
    assert.ok(duration >= 1000, `the intro lasts a moment (${duration} ms)`);

    const { clickAt, markAt } = await page.evaluate(() => ({ clickAt: performance.now(), markAt: window.__intro.markAt }));
    assert.ok(clickAt - markAt < duration - 200, `the test clicks while the intro still runs (${Math.round(clickAt - markAt)} ms in)`);
    await page.mouseClick(640, 450);
    await page.waitForGone('#intro', { timeout: 2_000 });
    const goneAt = await page.evaluate(() => window.__intro.goneAt);
    assert.ok(goneAt - clickAt < 700, `the intro leaves promptly after the click (${Math.round(goneAt - clickAt)} ms)`);
    assert.ok(goneAt - markAt < duration, 'it left before it would have ended by itself');

    // Usable: the core is loaded, its catalogue is shown and files can be added.
    await appReady(page);
    assert.equal(await page.text('#format-count'), String(core.formats().length));
    assert.equal(await page.text('#core-version'), `core ${core.version()}`);
    assert.match(await page.text('#app-version'), /^v\d+\.\d+\.\d+/);
    assert.match(await page.text(tid('privacy-statement')), /Files never leave your device/);
    await chooseFiles(page, [await files.file('first.txt', TEXT)]);
    await waitForReady(page, ['first.txt']);
    await files.remove();
  });

  e2e('a key press skips the intro too', async ({ ctx }) => {
    const page = await openWithIntro(ctx);
    await page.waitFor(introAnimations, [], { message: 'the intro animation to run' });
    await page.press('Escape');
    await page.waitForGone('#intro', { timeout: 2_000 });
    await appReady(page);
  });

  e2e('with reduced motion the intro is a static mark that fades away by itself', async ({ ctx }) => {
    const files = await tempDir();
    const page = await openWithIntro(ctx, { reducedMotion: 'reduce' });
    const mark = await page.waitFor(() => {
      const svg = document.querySelector('#intro svg.intro-mark');
      if (!svg) return false;
      const runner = svg.querySelector('.mk-runner');
      const moving = document
        .getAnimations()
        .filter((a) => a.effect && a.effect.target !== svg && svg.contains(a.effect.target));
      return { dash: runner.style.strokeDasharray, moving: moving.length };
    }, [], { message: 'the static intro mark' });
    assert.equal(mark.dash.replace(/,/g, ' ').replace(/\s+/g, ' '), '1000 1000', 'the whole mark is drawn at once');
    assert.equal(mark.moving, 0, 'no layer of the mark moves');
    // No input: it goes away on its own, quickly.
    await page.waitForGone('#intro', { timeout: 3_000 });
    const { markAt, goneAt } = await page.evaluate(() => window.__intro);
    assert.ok(goneAt - markAt < 1_500, `short static intro (${Math.round(goneAt - markAt)} ms)`);
    await appReady(page);
    // New cards do not animate in either.
    await chooseFiles(page, [await files.file('calm.txt', TEXT)]);
    await waitForReady(page, ['calm.txt']);
    const anims = await page.evaluate(() => document.querySelector('[data-testid="file-card"]').getAnimations().length);
    assert.equal(anims, 0);
    await files.remove();
  });

  e2e('the intro plays once per session', async ({ ctx }) => {
    const page = await openWithIntro(ctx);
    await page.waitFor(introAnimations, [], { message: 'the intro animation to run' });
    await page.mouseClick(640, 450);
    await appReady(page);
    await page.reload();
    await appReady(page);
    const seen = await page.evaluate(() => window.__intro.markAt);
    assert.equal(seen, null, 'no intro mark on the second load');
  });

  e2e('a slow core download shows byte progress while the app stays usable', async ({ ctx }) => {
    const page = await ctx.newPage();
    await page.addInitScript(`try { sessionStorage.setItem('convertible.intro', '1'); } catch {}`);
    // Serve the core to the page in small pieces with pauses (a slow network).
    await page.addInitScript(`
      const realFetch = window.fetch.bind(window);
      window.fetch = async (input, init) => {
        const response = await realFetch(input, init);
        if (!String(input).endsWith('convertible.wasm')) return response;
        const reader = response.body.getReader();
        const body = new ReadableStream({
          async pull(controller) {
            const { done, value } = await reader.read();
            if (done) return controller.close();
            for (let i = 0; i < value.length; i += 1 << 20) {
              await new Promise((r) => setTimeout(r, 60));
              controller.enqueue(value.subarray(i, i + (1 << 20)));
            }
          },
        });
        return new Response(body, { status: 200, headers: response.headers });
      };
    `);
    await page.goto(server().url);
    await page.waitForSelector(tid('core-status'));
    await page.waitFor(() => / of [0-9]/.test(document.querySelector('[data-testid="core-status"]').textContent), [], { message: 'byte progress ("x MB of y MB")' });
    const status = await page.evaluate(() => {
      const el = document.querySelector('[data-testid="core-status"]');
      return { role: el.getAttribute('role'), live: el.getAttribute('aria-live'), mark: Boolean(el.querySelector('svg.progress-mark')), text: el.textContent };
    });
    assert.equal(status.role, 'status');
    assert.equal(status.live, 'polite');
    assert.ok(status.mark, 'the brand progress mark');
    assert.match(status.text, /MB of \d/);
    // The shell works meanwhile: the formats browser opens with the catalogue.
    await page.waitFor(() => document.body.classList.contains('catalogue-ready'), [], { message: 'the catalogue before the core' });
    assert.equal(await page.evaluate(() => document.body.classList.contains('core-ready')), false, 'the core is still loading');
    await page.click(tid('open-formats'));
    await page.waitForSelector('dialog[data-testid="formats-dialog"][open] [data-testid^="format-"]');
    await page.press('Escape');
    // Then the core arrives and the status goes away.
    await appReady(page, { timeout: 60_000 });
    await page.waitForGone(`${tid('core-status')}:not([hidden])`, { timeout: 5_000 });
  }, { timeout: 120_000 });

  e2e('a failed core download offers Retry, and Retry starts the converter', async ({ ctx }) => {
    const files = await tempDir();
    const page = await ctx.newPage();
    await page.addInitScript(`try { sessionStorage.setItem('convertible.intro', '1'); } catch {}`);
    await page.addInitScript(`
      const realFetch = window.fetch.bind(window);
      let failed = false;
      window.fetch = (input, init) => {
        if (!failed && String(input).endsWith('convertible.wasm')) {
          failed = true;
          return Promise.reject(new TypeError('Failed to fetch'));
        }
        return realFetch(input, init);
      };
    `);
    await page.goto(server().url);
    await page.waitForSelector(`${tid('core-status')}[data-state="failed"]`);
    assert.match(await page.text(tid('core-status')), /could not be loaded/);
    // A file added now waits for the converter instead of failing.
    await chooseFiles(page, [await files.file('notes.txt', TEXT)]);
    await page.click(tid('core-retry'));
    await appReady(page, { timeout: 60_000 });
    await waitForReady(page, ['notes.txt'], { timeout: 30_000 });
    await files.remove();
  });

  e2e('the home screen loads cleanly: same-origin requests only, no errors', async ({ ctx }) => {
    const page = await openApp(ctx, server());
    assert.deepEqual(await cards(page), []);
    assert.equal(await page.evaluate(() => document.getElementById('queue').hidden), true);
    // Clean-run assertions (requests, console, CSP) run after every test.
  });
});

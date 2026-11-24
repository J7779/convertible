// Responsive layout at 320, 375, 768 and 1440 px in dark and light: no
// horizontal overflow on the home screen, the queue, the picker and the
// sheets; the colour scheme is applied.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { chooseFiles, convert, openApp, tid, waitForReady } from './lib/app.mjs';
import { TEXT, allBytes, tempDir } from './lib/fixtures.mjs';
import { e2e, server, suite } from './lib/harness.mjs';

const WIDTHS = [320, 375, 768, 1440];
const SCHEMES = ['dark', 'light'];

/** Horizontal overflow of the page and of open dialogs, with the widest offenders. */
function overflow() {
  const vw = document.documentElement.clientWidth;
  const offenders = [];
  for (const el of document.querySelectorAll('body *')) {
    if (el.closest('[hidden], dialog:not([open])')) continue;
    const r = el.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) continue;
    if (r.right > vw + 1 || r.left < -1) {
      // Elements clipped by a scrolling/clipping ancestor do not overflow the page.
      let clipped = false;
      for (let p = el.parentElement; p && p !== document.body; p = p.parentElement) {
        const cs = getComputedStyle(p);
        if (/(hidden|auto|scroll|clip)/.test(cs.overflowX)) {
          const pr = p.getBoundingClientRect();
          if (pr.right <= vw + 1 && pr.left >= -1) clipped = true;
          break;
        }
      }
      if (!clipped && !el.classList.contains('skip-link') && !el.classList.contains('sr-only')) offenders.push(`${el.tagName.toLowerCase()}.${String(el.className.baseVal ?? el.className).split(' ')[0]} ${Math.round(r.left)}..${Math.round(r.right)}`);
    }
  }
  const dialogs = [...document.querySelectorAll('dialog[open]')].map((d) => ({ scroll: d.scrollWidth, client: d.clientWidth, right: d.getBoundingClientRect().right }));
  return {
    vw,
    doc: document.documentElement.scrollWidth,
    body: document.body.scrollWidth,
    dialogs,
    offenders: offenders.slice(0, 8),
  };
}

function assertNoOverflow(o, where) {
  assert.ok(o.doc <= o.vw, `${where}: page scrolls sideways (${o.doc} > ${o.vw})`);
  assert.ok(o.body <= o.vw, `${where}: body wider than the viewport (${o.body} > ${o.vw})`);
  for (const d of o.dialogs) {
    assert.ok(d.scroll <= d.client + 1, `${where}: dialog content overflows (${d.scroll} > ${d.client})`);
    assert.ok(d.right <= o.vw + 1, `${where}: dialog wider than the viewport`);
  }
  assert.deepEqual(o.offenders, [], `${where}: elements outside the viewport`);
}

function colours() {
  const parse = (c) => c.match(/[\d.]+/g).slice(0, 3).map(Number);
  const lum = ([r, g, b]) => (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
  const cs = getComputedStyle(document.body);
  return { bg: lum(parse(cs.backgroundColor)), fg: lum(parse(cs.color)) };
}

suite('responsive layout', () => {
  for (const width of WIDTHS) {
    for (const scheme of SCHEMES) {
      e2e(`${width} px, ${scheme}: no horizontal overflow anywhere`, async ({ ctx }) => {
        const files = await tempDir();
        const long = await files.file('a-really-long-file-name-that-keeps-going-and-going-without-any-spaces-at-all.txt', TEXT);
        const bin = await files.file('bytes.bin', allBytes());
        const mobile = width < 768;
        const page = await openApp(ctx, server(), { width, height: mobile ? 740 : 900, mobile, colorScheme: scheme });
        assert.equal(await page.evaluate(() => innerWidth), width);

        const c = await page.evaluate(colours);
        if (scheme === 'dark') assert.ok(c.bg < 0.25 && c.fg > 0.6, `dark scheme (${JSON.stringify(c)})`);
        else assert.ok(c.bg > 0.75 && c.fg < 0.4, `light scheme (${JSON.stringify(c)})`);
        assertNoOverflow(await page.evaluate(overflow), 'home');

        await chooseFiles(page, [long, bin]);
        const names = ['a-really-long-file-name-that-keeps-going-and-going-without-any-spaces-at-all.txt', 'bytes.bin'];
        await waitForReady(page, names);
        assertNoOverflow(await page.evaluate(overflow), 'queue');

        // Card options open, then a finished card.
        await page.click({ card: names[1], sel: `${tid('advanced')} > summary` });
        await page.waitFor(() => document.querySelector('[data-testid="advanced"][open]'));
        assertNoOverflow(await page.evaluate(overflow), 'options');
        await convert(page, names[0]);
        assertNoOverflow(await page.evaluate(overflow), 'result');

        // Picker, formats browser and settings.
        await page.click({ card: names[1], sel: tid('target-picker') });
        await page.waitForSelector('dialog[data-testid="picker-dialog"][open]');
        assertNoOverflow(await page.evaluate(overflow), 'picker');
        await page.click(tid('picker-close'));
        await page.waitForGone('dialog[data-testid="picker-dialog"][open]');

        await page.click(tid('open-formats'));
        await page.waitForSelector('dialog[data-testid="formats-dialog"][open]');
        assertNoOverflow(await page.evaluate(overflow), 'formats');
        await page.click(tid('formats-dialog-close'));
        await page.waitForGone('dialog[data-testid="formats-dialog"][open]');

        await page.click(tid('open-settings'));
        await page.waitForSelector('dialog[data-testid="settings-dialog"][open]');
        assertNoOverflow(await page.evaluate(overflow), 'settings');

        // Touch-sized targets on phones.
        if (mobile) {
          const small = await page.evaluate(() =>
            [...document.querySelectorAll('dialog[open] button, .topnav button:not([hidden]), [data-testid="file-card"] button')]
              .filter((b) => b.getClientRects().length && !b.closest('[hidden]'))
              .map((b) => ({ label: b.getAttribute('aria-label') || b.textContent.trim(), h: b.getBoundingClientRect().height, w: b.getBoundingClientRect().width }))
              .filter((b) => b.h < 32 || b.w < 32),
          );
          assert.deepEqual(small, [], 'buttons are large enough to tap');
        }
        await files.remove();
      });
    }
  }
});

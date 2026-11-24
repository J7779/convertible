// Keyboard only: the whole main flow (choose a file, pick a target, convert,
// save) without a pointer, with a visible focus indicator at every stop.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { openApp, outputName, waitForCard, waitForReady } from './lib/app.mjs';
import { TEXT, tempDir } from './lib/fixtures.mjs';
import { core, e2e, server, suite } from './lib/harness.mjs';

/** The focused element and whether its focus is visibly indicated. */
function focusInfo() {
  const el = document.activeElement;
  if (!el || el === document.body) return null;
  const ring = (node) => {
    const cs = getComputedStyle(node);
    return cs.outlineStyle !== 'none' && parseFloat(cs.outlineWidth) > 0;
  };
  // Segmented radios draw the ring on their label text.
  const visible = ring(el) || (el.matches('.segment input') && ring(el.nextElementSibling));
  const r = el.getBoundingClientRect();
  return {
    testid: el.dataset.testid ?? null,
    label: (el.getAttribute('aria-label') || el.textContent || el.id || el.tagName).trim().replace(/\s+/g, ' ').slice(0, 40),
    focusVisible: el.matches(':focus-visible'),
    ring: visible,
    inView: r.bottom > 0 && r.top < innerHeight && r.width > 0,
    inCard: el.closest('[data-testid="file-card"]')?.querySelector('.file-name')?.textContent ?? null,
  };
}

/** Presses Tab until `predicate(focusInfo)` holds, checking every stop on the way. */
async function tabTo(page, predicate, what, { max = 30, back = false } = {}) {
  const stops = [];
  for (let i = 0; i < max; i += 1) {
    await page.press(back ? 'Shift+Tab' : 'Tab');
    const f = await page.evaluate(focusInfo);
    if (!f) continue;
    stops.push(f.label);
    assert.ok(f.focusVisible && f.ring, `focus on "${f.label}" is visible (focus-visible ${f.focusVisible}, ring ${f.ring})`);
    assert.ok(f.inView, `focused "${f.label}" is scrolled into view`);
    if (predicate(f)) return f;
  }
  assert.fail(`${what} not reached with Tab; stops: ${stops.join(' | ')}`);
}

suite('keyboard', () => {
  e2e('the main flow works with the keyboard alone, with visible focus', async ({ ctx }) => {
    const files = await tempDir();
    const file = await files.file('keys.txt', TEXT);
    const page = await openApp(ctx, server());

    // The first stop is the skip link, which becomes visible and works.
    await page.press('Tab');
    await page.waitFor(() => {
      const el = document.activeElement;
      const r = el.getBoundingClientRect();
      return el.classList.contains('skip-link') && r.top >= 0 && r.bottom <= innerHeight && r.height > 0;
    }, [], { message: 'the skip link to slide into view when focused' });
    await page.press('Enter');
    await page.waitFor(() => document.activeElement?.id === 'main', [], { message: 'the skip link to move focus to the main content' });

    // Tab to "Choose files", Enter opens the native chooser.
    await tabTo(page, (f) => f.testid === 'choose-files', 'Choose files');
    const chooser = await page.expectFileChooser([file]);
    await page.press('Enter');
    await chooser.done;
    await waitForReady(page, ['keys.txt']);

    // Tab to the card's target button; Enter opens the picker with the search focused.
    await tabTo(page, (f) => f.testid === 'target-picker' && f.inCard === 'keys.txt', 'the target button');
    await page.press('Enter');
    await page.waitForSelector('dialog[data-testid="picker-dialog"][open]');
    // The search field has focus with a mouse; on touch-only devices the close
    // button does and Tab moves on to the search field.
    const first = await page.waitFor(() => ['picker-search', 'picker-close'].find((id) => document.activeElement?.dataset.testid === id), [], { message: 'focus in the picker' });
    assert.equal(first, (await page.evaluate(() => matchMedia('(pointer: fine)').matches)) ? 'picker-search' : 'picker-close');
    if (first !== 'picker-search') await tabTo(page, (f) => f.testid === 'picker-search', 'the search field');
    await page.type('hex');
    await page.press('Enter');
    await page.waitForGone('dialog[data-testid="picker-dialog"][open]');
    await page.waitFor(() => document.activeElement?.dataset.testid === 'target-picker', [], { message: 'focus returns to the target button' });
    await page.waitFor(() => document.querySelector('[data-testid="target-picker"] .badge')?.textContent.trim() === 'HEX', [], { message: 'the target to become HEX' });
    const back = await page.evaluate(focusInfo);
    assert.ok(back.focusVisible && back.ring, 'focus is visible after the dialog closes');

    // Tab to Convert; Enter converts.
    await tabTo(page, (f) => f.testid === 'convert' && f.inCard === 'keys.txt', 'Convert');
    await page.press('Enter');
    await waitForCard(page, 'keys.txt', 'done');
    // Focus stays inside the card after it re-renders.
    await page.waitFor(() => Boolean(document.activeElement?.closest('[data-testid="file-card"]')), [], { message: 'focus kept in the card' });

    // Tab (or Shift+Tab) to Save; Enter downloads the right bytes.
    const after = ctx.downloadsSoFar();
    const here = await page.evaluate(focusInfo);
    if (here.testid !== 'save') await tabTo(page, (f) => f.testid === 'save', 'Save');
    await page.press('Enter');
    const ext = core.convert(new Uint8Array(Buffer.from(TEXT)), { to: 'hex', filename: 'keys.txt' }).extension;
    const d = await ctx.waitForDownload(outputName('keys.txt', ext), { after });
    assert.equal(Buffer.from(d.bytes.toString('latin1').replace(/\s+/g, ''), 'hex').toString(), TEXT);
    await files.remove();
  });

  e2e('Ctrl+O opens the file chooser; Escape closes sheets and returns focus', async ({ ctx }) => {
    const files = await tempDir();
    const file = await files.file('shortcut.txt', TEXT);
    const page = await openApp(ctx, server());
    const chooser = await page.expectFileChooser([file]);
    await page.press('Control+o');
    await chooser.done;
    await waitForReady(page, ['shortcut.txt']);

    await tabTo(page, (f) => f.testid === 'open-settings', 'Settings', { back: true });
    await page.press('Enter');
    await page.waitForSelector('dialog[data-testid="settings-dialog"][open]');
    // Focus is trapped in the modal: Tab stays inside it.
    for (let i = 0; i < 12; i += 1) {
      await page.press('Tab');
      const inside = await page.evaluate(() => Boolean(document.activeElement?.closest('dialog[open]')) || document.activeElement === document.body);
      assert.ok(inside, 'focus stays in the settings sheet');
    }
    await page.press('Escape');
    await page.waitForGone('dialog[data-testid="settings-dialog"][open]');
    await page.waitFor(() => document.activeElement?.dataset.testid === 'open-settings', [], { message: 'focus returns to Settings' });
    await files.remove();
  });
});

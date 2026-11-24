// The target picker: it lists exactly what the core offers, grouped; search;
// keyboard selection; dismissal; and the Compress action.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { card, chooseFiles, chooseTarget, convert, openApp, outputName, save, tid, waitForReady } from './lib/app.mjs';
import { TEXT, tempDir } from './lib/fixtures.mjs';
import { core, e2e, server, suite } from './lib/harness.mjs';

const { CATEGORY_LABELS } = await import('../src/js/model/targets.js');

const PICKER = 'dialog[data-testid="picker-dialog"][open]';

/** The open picker as rendered: groups with their options, and the active option. */
function readPicker() {
  const dlg = document.querySelector('dialog[data-testid="picker-dialog"][open]');
  if (!dlg) return null;
  const groups = [...dlg.querySelectorAll('.picker-group-wrap')].map((w) => ({
    label: w.querySelector('.picker-group-label').textContent,
    ids: [...w.querySelectorAll('[role="option"]')].map((o) => o.dataset.value),
  }));
  const search = dlg.querySelector('[data-testid="picker-search"]');
  const active = search.getAttribute('aria-activedescendant');
  return {
    title: dlg.querySelector('#picker-title').textContent,
    groups,
    options: [...dlg.querySelectorAll('[role="option"]')].map((o) => o.dataset.value),
    active: active ? document.getElementById(active)?.dataset.value : null,
    empty: !dlg.querySelector('.picker-empty').hidden,
    focused: document.activeElement === search ? 'search' : document.activeElement?.dataset.testid ?? null,
    finePointer: matchMedia('(pointer: fine)').matches,
  };
}

async function openPickerFor(page, name) {
  await page.click({ card: name, sel: tid('target-picker') });
  await page.waitForSelector(PICKER);
  const picker = await page.evaluate(readPicker);
  // Without a fine pointer the close button has focus; Tab reaches the search.
  if (picker.focused !== 'search') {
    await page.press('Tab');
    await page.waitFor(() => document.activeElement?.dataset.testid === 'picker-search', [], { message: 'Tab to the search field' });
  }
  return picker;
}

async function setupText(ctx) {
  const files = await tempDir();
  const path = await files.file('notes.txt', TEXT);
  const page = await openApp(ctx, server());
  await chooseFiles(page, [path]);
  await waitForReady(page, ['notes.txt']);
  const source = core.detect(new Uint8Array(Buffer.from(TEXT)), 'notes.txt');
  return { files, page, source };
}

suite('target picker', () => {
  e2e('lists exactly the targets the core offers, grouped by category', async ({ ctx }) => {
    const { files, page, source } = await setupText(ctx);
    const targets = core.targets(source);
    await page.click({ card: 'notes.txt', sel: tid('target-picker') });
    await page.waitForSelector(PICKER);
    const picker = await page.evaluate(readPicker);
    assert.match(picker.title, new RegExp(`Convert ${source.toUpperCase()} to`));
    // With a mouse the search field takes focus; on touch-only devices the close
    // button does, so the on-screen keyboard does not cover the list.
    assert.equal(picker.focused, picker.finePointer ? 'search' : 'picker-close', 'initial focus');

    // Every target exactly once outside "Suggested", nothing else.
    const plain = picker.groups.filter((g) => g.label !== 'Suggested').flatMap((g) => g.ids);
    assert.deepEqual([...plain].sort(), targets.map((t) => t.id).sort());

    // "Same format, smaller" holds exactly the compress entry, first.
    const compress = targets.filter((t) => t.compress).map((t) => t.id);
    if (compress.length) assert.deepEqual(picker.groups[0], { label: 'Same format, smaller', ids: compress });

    // Category groups: the source's category first, each holding that
    // category's targets in the core's relevance order.
    const regular = targets.filter((t) => !t.compress);
    const categories = [...new Set(regular.map((t) => t.category))];
    const categoryGroups = picker.groups.filter((g) => g.label !== 'Suggested' && g.label !== 'Same format, smaller');
    assert.equal(categoryGroups.length, categories.length);
    for (const g of categoryGroups) {
      const category = Object.keys(CATEGORY_LABELS).find((c) => CATEGORY_LABELS[c] === g.label) ?? g.label.toLowerCase();
      assert.deepEqual(g.ids, regular.filter((t) => t.category === category).map((t) => t.id), `group ${g.label}`);
    }
    const sourceCategory = core.formats().find((f) => f.id === source).category;
    if (categories.includes(sourceCategory)) assert.equal(categoryGroups[0].label, CATEGORY_LABELS[sourceCategory]);

    // Long lists get a short "Suggested" group.
    const suggested = picker.groups.find((g) => g.label === 'Suggested');
    if (regular.length > 10) {
      assert.ok(suggested && suggested.ids.length > 0 && suggested.ids.length <= 6);
      for (const id of suggested.ids) assert.ok(regular.some((t) => t.id === id));
    } else assert.equal(suggested, undefined);

    // The current target is marked selected and active.
    const current = (await card(page, 'notes.txt')).target.toLowerCase();
    assert.equal(picker.active, current);
    assert.equal(await page.evaluate((c) => document.querySelector(`dialog[open] [data-value="${c}"]`).getAttribute('aria-selected'), current), 'true');
    await page.click(tid('picker-close'));
    await page.waitForGone(PICKER);
    await files.remove();
  });

  e2e('search filters the list; arrow keys and Enter choose; no match says so', async ({ ctx }) => {
    const { files, page, source } = await setupText(ctx);
    const targets = core.targets(source).filter((t) => !t.compress);
    await openPickerFor(page, 'notes.txt');

    // Searching for a target's id puts it first and makes it active.
    const wanted = targets.find((t) => t.id === 'hex') ?? targets.at(-1);
    await page.type(wanted.id);
    let picker = await page.evaluate(readPicker);
    assert.equal(picker.options[0], wanted.id);
    assert.equal(picker.active, wanted.id);
    assert.ok(picker.options.length < targets.length + 1, 'the list is filtered');
    for (const id of picker.options) assert.ok(core.targets(source).some((t) => t.id === id));

    // Arrow keys move the active option; Enter picks it.
    if (picker.options.length > 1) {
      await page.press('ArrowDown');
      picker = await page.evaluate(readPicker);
      assert.equal(picker.active, picker.options[1]);
      await page.press('ArrowUp');
      picker = await page.evaluate(readPicker);
      assert.equal(picker.active, picker.options[0]);
    }
    await page.press('Enter');
    await page.waitForGone(PICKER);
    await page.waitFor((label) => document.querySelector('[data-testid="target-picker"] .badge')?.textContent.trim() === label, [wanted.id.toUpperCase()]);
    // Focus returns to the control that opened the picker.
    await page.waitFor(() => document.activeElement?.dataset.testid === 'target-picker', [], { message: 'focus back on the target button' });

    // Clearing the search and moving down chooses the next target in the list.
    await openPickerFor(page, 'notes.txt');
    picker = await page.evaluate(readPicker);
    const index = picker.options.indexOf(picker.active);
    await page.press('ArrowDown');
    await page.press('Enter');
    await page.waitForGone(PICKER);
    const next = picker.options[Math.min(index + 1, picker.options.length - 1)];
    await page.waitFor((label) => document.querySelector('[data-testid="target-picker"] .badge')?.textContent.trim() === label, [next.toUpperCase()]);

    // A search without results.
    await openPickerFor(page, 'notes.txt');
    await page.type('zzqqxxnotaformat');
    picker = await page.evaluate(readPicker);
    assert.equal(picker.options.length, 0);
    assert.equal(picker.empty, true);
    assert.equal(await page.text(`${PICKER} .picker-empty`), 'No matching formats.');

    // Escape first clears the search field (platform behaviour of
    // type=search), a second Escape closes without changing anything.
    await page.press('Escape');
    assert.equal(await page.evaluate(() => document.querySelector('[data-testid="picker-search"]').value), '');
    assert.equal(await page.evaluate(() => Boolean(document.querySelector('dialog[data-testid="picker-dialog"][open]'))), true);
    await page.press('Escape');
    await page.waitForGone(PICKER);
    assert.equal((await card(page, 'notes.txt')).target, next.toUpperCase());
    await page.waitFor(() => document.activeElement?.dataset.testid === 'target-picker', [], { message: 'focus back on the target button' });
    await files.remove();
  });

  e2e('Compress offers only same-format and archive targets and re-encodes the file', async ({ ctx }) => {
    const { files, page, source } = await setupText(ctx);
    const offered = core.targets(source).filter((t) => t.compress || t.category === 'archive');
    if (!offered.length) {
      assert.equal(await page.count(tid('compress')), 0, 'no Compress button without a compress target');
      await files.remove();
      return;
    }
    await page.click({ card: 'notes.txt', sel: tid('compress') });
    await page.waitForSelector(PICKER);
    const picker = await page.evaluate(readPicker);
    assert.equal(picker.title, 'Compress notes.txt');
    assert.deepEqual([...picker.options].sort(), offered.map((t) => t.id).sort());

    const same = offered.find((t) => t.compress) ?? offered[0];
    await page.click(`${PICKER} ${tid(`target-option-${same.id}`)}`);
    await page.waitForGone(PICKER);
    await page.waitFor((label) => document.querySelector('[data-testid="target-picker"] .badge')?.textContent.trim() === label, [same.id.toUpperCase()]);
    if (same.compress) assert.equal((await card(page, 'notes.txt')).targetName, `${same.name}, compressed`);

    await convert(page, 'notes.txt');
    const expected = core.convert(new Uint8Array(Buffer.from(TEXT)), { from: source, to: same.id, filename: 'notes.txt' });
    const d = await save(page, ctx, 'notes.txt', outputName('notes.txt', expected.extension));
    assert.ok(d.bytes.equals(Buffer.from(expected.data)), 'the download equals what the core produces');
    if (same.id === source && source === 'txt') assert.equal(new TextDecoder().decode(d.bytes), TEXT, 'the text survives');
    await files.remove();
  });

  e2e('choosing a different target in the picker by pointer', async ({ ctx }) => {
    const { files, page, source } = await setupText(ctx);
    const targets = core.targets(source).filter((t) => !t.compress);
    const pick = targets.at(-1);
    await chooseTarget(page, 'notes.txt', pick.id);
    assert.equal((await card(page, 'notes.txt')).targetName, pick.name);
    await files.remove();
  });
});

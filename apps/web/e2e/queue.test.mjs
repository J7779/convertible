// Adding files: the picker, drag-and-drop, the multi-file queue with the
// formats the core detects, "set all files of this format", Convert all,
// Save all, remove and clear.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { card, cards, chooseFiles, chooseTarget, openApp, outputName, tid, waitForCard, waitForReady } from './lib/app.mjs';
import { TEXT, allBytes, tempDir } from './lib/fixtures.mjs';
import { core, e2e, server, suite } from './lib/harness.mjs';

const { formatBytes } = await import('../src/js/model/format.js');

/** What the badge of a card for this file should say, according to the core. */
function expectedBadge(bytes, name) {
  const id = core.detect(new Uint8Array(bytes.subarray(0, 64 * 1024)), name);
  return id ? id.toUpperCase() : 'FILE';
}

/** Default target the app should preselect (first same-category target, else the first). */
function defaultTarget(source) {
  const info = core.formats().find((f) => f.id === source);
  const regular = core.targets(source).filter((t) => !t.compress);
  return (regular.find((t) => t.category === info?.category) ?? regular[0])?.id ?? null;
}

const INPUTS = {
  'notes.txt': Buffer.from(TEXT),
  'all-bytes.bin': allBytes(),
  'encoded.b64': Buffer.from(`${Buffer.from(TEXT).toString('base64')}\n`),
  'digits.hex': Buffer.from(`${Buffer.from('hex digits').toString('hex')}\n`),
};

suite('file queue', () => {
  e2e('the file picker adds several files; each card shows the detected format and the default target', async ({ ctx }) => {
    const files = await tempDir();
    const paths = [];
    for (const [name, data] of Object.entries(INPUTS)) paths.push(await files.file(name, data));
    const page = await openApp(ctx, server());
    await chooseFiles(page, paths);
    await waitForReady(page, Object.keys(INPUTS));

    const list = await cards(page);
    assert.deepEqual(list.map((c) => c.name), Object.keys(INPUTS), 'cards appear in the order chosen');
    for (const [name, data] of Object.entries(INPUTS)) {
      const c = list.find((x) => x.name === name);
      const badge = expectedBadge(data, name);
      assert.equal(c.badge, badge, `badge of ${name}`);
      assert.equal(c.unknown, badge === 'FILE', `${name} is flagged as unrecognised only when the core cannot tell`);
      const source = badge === 'FILE' ? 'bin' : badge.toLowerCase();
      assert.equal(c.target, defaultTarget(source)?.toUpperCase(), `default target of ${name}`);
      const sub = await page.text({ card: name, sel: '.file-sub' });
      assert.ok(sub.startsWith(formatBytes(data.length)), `${name} shows its size (${sub})`);
    }
    assert.equal(await page.text('#queue-count'), `${paths.length} files`);
    assert.equal(await page.text(tid('convert-all')), `Convert ${paths.length} files`);
    await files.remove();
  });

  e2e('dropping files anywhere shows the drop target and adds them', async ({ ctx }) => {
    const files = await tempDir();
    const a = await files.file('dropped-a.txt', 'first dropped file\n');
    const b = await files.file('dropped-b.txt', 'second dropped file\n');
    const page = await openApp(ctx, server());
    const drag = await page.dropFiles(tid('dropzone'), [a, b]);
    const during = await page.evaluate(() => ({ overlay: !document.getElementById('drop-overlay').hidden, dragging: document.body.classList.contains('is-dragging') }));
    assert.deepEqual(during, { overlay: true, dragging: true });
    await drag.drop();
    await waitForReady(page, ['dropped-a.txt', 'dropped-b.txt']);
    const after = await page.evaluate(() => ({ overlay: !document.getElementById('drop-overlay').hidden, dragging: document.body.classList.contains('is-dragging') }));
    assert.deepEqual(after, { overlay: false, dragging: false });

    // A second drop onto the queue (not the drop zone) adds to it.
    const c = await files.file('dropped-c.bin', allBytes());
    const drag2 = await page.dropFiles('#cards', [c]);
    await drag2.drop();
    await waitForReady(page, ['dropped-c.bin']);
    assert.equal((await cards(page)).length, 3);

    await files.remove();
  });

  e2e('folders: "Choose a folder" and dropping a folder add every file inside, recursively', async ({ ctx }) => {
    const files = await tempDir();
    const folder = path.join(files.dir, 'album');
    await mkdir(path.join(folder, 'nested', 'deeper'), { recursive: true });
    await writeFile(path.join(folder, 'top.txt'), 'top level\n');
    await writeFile(path.join(folder, 'nested', 'middle.bin'), allBytes());
    await writeFile(path.join(folder, 'nested', 'deeper', 'bottom.txt'), 'deep down\n');
    // System clutter is ignored.
    await writeFile(path.join(folder, '.DS_Store'), 'x');
    await writeFile(path.join(folder, 'nested', 'Thumbs.db'), 'x');
    const expected = ['top.txt', 'middle.bin', 'bottom.txt'];

    const page = await openApp(ctx, server());
    const chooser = await page.expectFileChooser([folder]);
    await page.click(tid('choose-folder'));
    await chooser.done;
    await waitForReady(page, expected);
    assert.deepEqual((await cards(page)).map((c) => c.name).sort(), [...expected].sort());

    await page.click(tid('clear-all'));
    await page.waitFor(() => document.getElementById('queue').hidden);
    const drag = await page.dropFiles(tid('dropzone'), [folder]);
    await drag.drop();
    await waitForReady(page, expected);
    assert.deepEqual((await cards(page)).map((c) => c.name).sort(), [...expected].sort());
    await files.remove();
  });

  e2e('one control sets the target of every file of a format; Convert all and Save all', async ({ ctx }) => {
    const files = await tempDir();
    const inputs = { 'one.txt': Buffer.from('first text file\n'), 'two.txt': Buffer.from('second text file, a bit longer\n'), 'raw.bin': allBytes() };
    const paths = [];
    for (const [name, data] of Object.entries(inputs)) paths.push(await files.file(name, data));
    const page = await openApp(ctx, server());
    await chooseFiles(page, paths);
    await waitForReady(page, Object.keys(inputs));

    // Only formats shared by two or more files get a bulk control.
    const txtSource = core.detect(new Uint8Array(inputs['one.txt']), 'one.txt');
    await page.waitForSelector(tid(`bulk-${txtSource}`));
    assert.equal(await page.count('[data-testid^="bulk-"]:not([data-testid="bulk-target"])'), 1);
    await page.waitFor((id, re) => new RegExp(re, 'i').test(document.querySelector(`[data-testid="bulk-${id}"]`)?.textContent.replace(/\s+/g, ' ') ?? ''), [txtSource, `All 2 ${txtSource.toUpperCase()} files to`], { message: 'the group label' });

    // Choose a target for the group from the catalogue (hex is universal).
    const target = core.targets(txtSource).find((t) => t.id === 'hex') ? 'hex' : core.targets(txtSource).find((t) => !t.compress).id;
    await page.click({ within: tid(`bulk-${txtSource}`), sel: tid('bulk-target') });
    await page.waitForSelector('dialog[data-testid="picker-dialog"][open]');
    assert.match(await page.text('#picker-title'), new RegExp(`Convert all ${txtSource.toUpperCase()} files to`));
    await page.focus(tid('picker-search'));
    await page.type(target);
    await page.click(`dialog[open] ${tid(`target-option-${target}`)}`);
    await page.waitForGone('dialog[data-testid="picker-dialog"][open]');
    await page.waitFor((t) => [...document.querySelectorAll('[data-testid="file-card"]')].filter((li) => li.querySelector('[data-testid="target-picker"] .badge')?.textContent.trim() === t).length >= 2, [target.toUpperCase()]);
    assert.equal((await card(page, 'one.txt')).target, target.toUpperCase());
    assert.equal((await card(page, 'two.txt')).target, target.toUpperCase());
    assert.notEqual((await card(page, 'raw.bin')).badge, txtSource.toUpperCase(), 'other formats are untouched');
    // The group control shows the shared target (updated on the next frame).
    await page.waitFor((id, t) => document.querySelector(`[data-testid="bulk-${id}"] [data-testid="bulk-target"]`)?.textContent.trim() === t, [txtSource, target.toUpperCase()], { message: 'the group control to show the new target' });

    // The binary file goes to Base64; then everything converts at once.
    await chooseTarget(page, 'raw.bin', 'base64');
    await page.click(tid('convert-all'));
    for (const name of Object.keys(inputs)) await waitForCard(page, name, 'done', { timeout: 30_000 });
    // The toolbar is updated on the next frame.
    await page.waitFor(() => document.querySelector('[data-testid="save-all"]:not([hidden])')?.textContent.trim() === 'Save all (3)', [], { message: 'Save all (3)' });

    // Save all: one download per result, with the right names and bytes.
    const before = ctx.downloadsSoFar();
    await page.click(tid('save-all'));
    const expected = {
      [outputName('one.txt', core.convert(new Uint8Array(inputs['one.txt']), { from: txtSource, to: target }).extension)]: inputs['one.txt'],
      [outputName('two.txt', core.convert(new Uint8Array(inputs['two.txt']), { from: txtSource, to: target }).extension)]: inputs['two.txt'],
      'raw.b64': inputs['raw.bin'],
    };
    for (const [name, original] of Object.entries(expected)) {
      const d = await ctx.waitForDownload(name, { after: before });
      const text = d.bytes.toString('latin1');
      const decoded = name.endsWith('.b64') ? Buffer.from(text, 'base64') : Buffer.from(text.replace(/[^0-9a-f]/gi, ''), 'hex');
      assert.ok(decoded.equals(original), `${name} decodes to its input`);
    }
    await files.remove();
  });

  e2e('removing a card and clearing the queue', async ({ ctx }) => {
    const files = await tempDir();
    const a = await files.file('keep.txt', 'keep me\n');
    const b = await files.file('drop.txt', 'remove me\n');
    const page = await openApp(ctx, server());
    await chooseFiles(page, [a, b]);
    await waitForReady(page, ['keep.txt', 'drop.txt']);
    await page.click({ card: 'drop.txt', sel: tid('remove') });
    await page.waitFor(() => document.querySelectorAll('[data-testid="file-card"]').length === 1);
    assert.deepEqual((await cards(page)).map((c) => c.name), ['keep.txt']);
    // Focus moves to a neighbour, not to the top of the page.
    await page.waitFor(() => Boolean(document.activeElement.closest('[data-testid="file-card"]')), [], { message: 'focus to move to the remaining card' });
    await page.waitFor(() => document.getElementById('queue-count').textContent === '1 file', [], { message: 'the count to read 1 file' });

    // Files can be added again after removal, then everything cleared.
    await chooseFiles(page, [b]);
    await waitForReady(page, ['drop.txt']);
    await page.click(tid('clear-all'));
    await page.waitFor(() => document.getElementById('queue').hidden && !document.querySelector('#cards li'));
    assert.equal(await page.evaluate(() => document.activeElement.dataset.testid), 'choose-files');
    assert.equal(await page.evaluate(() => document.body.classList.contains('has-files')), false);
    await files.remove();
  });

  e2e('a dropped file keeps its exact bytes through a conversion', async ({ ctx }) => {
    const files = await tempDir();
    const data = await readFile(await files.file('payload.bin', allBytes()));
    const page = await openApp(ctx, server());
    const drag = await page.dropFiles(tid('dropzone'), [`${files.dir}/payload.bin`]);
    await drag.drop();
    await waitForReady(page, ['payload.bin']);
    await chooseTarget(page, 'payload.bin', 'hex');
    await page.click({ card: 'payload.bin', sel: tid('convert') });
    await waitForCard(page, 'payload.bin', 'done');
    const after = ctx.downloadsSoFar();
    await page.click({ card: 'payload.bin', sel: tid('save') });
    const ext = core.convert(new Uint8Array(data), { from: 'bin', to: 'hex' }).extension;
    const d = await ctx.waitForDownload(`payload.${ext}`, { after });
    assert.ok(Buffer.from(d.bytes.toString('latin1').replace(/\s+/g, ''), 'hex').equals(data));
    await files.remove();
  });
});

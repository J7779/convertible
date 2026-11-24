// The error path: a file that fails to decode shows a friendly error, the
// queue survives, and the other files still convert.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { card, cards, chooseFiles, chooseTarget, convert, openApp, save, tid, waitForCard, waitForReady } from './lib/app.mjs';
import { TEXT, tempDir } from './lib/fixtures.mjs';
import { core, e2e, server, suite } from './lib/harness.mjs';

const { friendlyMessage, statusTitle } = await import('../src/js/model/format.js');

/** A file the core recognises by its content but cannot decode. */
const BROKEN = Buffer.from('@@@@!!');
const BROKEN_NAME = 'broken.b64';

suite('errors', () => {
  e2e('a file that cannot be decoded shows a friendly error; the queue survives', async ({ ctx }) => {
    const source = core.detect(new Uint8Array(BROKEN), BROKEN_NAME);
    const targets = core.targets(source);
    const failing = targets.find((t) => !t.compress && core.convert(new Uint8Array(BROKEN), { from: source, to: t.id, filename: BROKEN_NAME }).status !== 0);
    assert.ok(failing, 'the core rejects this input');
    const expected = core.convert(new Uint8Array(BROKEN), { from: source, to: failing.id, filename: BROKEN_NAME });

    const files = await tempDir();
    const bad = await files.file(BROKEN_NAME, BROKEN);
    const good = await files.file('good.txt', TEXT);
    const page = await openApp(ctx, server());
    await chooseFiles(page, [bad, good]);
    await waitForReady(page, [BROKEN_NAME, 'good.txt']);
    await chooseTarget(page, BROKEN_NAME, failing.id);
    await page.click({ card: BROKEN_NAME, sel: tid('convert') });
    await waitForCard(page, BROKEN_NAME, 'failed');

    // Friendly: a plain title and the core's reason as a sentence, as an alert.
    const box = await page.withElement({ card: BROKEN_NAME, sel: tid('error') }, (el) => ({
      role: el.getAttribute('role'),
      title: el.querySelector('.error-title').textContent,
      message: el.querySelector('.error-message')?.textContent ?? '',
    }));
    assert.deepEqual(box, { role: 'alert', title: statusTitle(expected.status), message: friendlyMessage(expected.message) });
    assert.match(box.message, /^[A-Z].*\.$/, 'a capitalised sentence');
    assert.doesNotMatch(box.title + box.message, /undefined|null|NaN|\[object|Error:/);
    await page.waitForSelector({ card: BROKEN_NAME, sel: tid('retry') });
    await page.waitForSelector({ card: BROKEN_NAME, sel: tid('change-format') });

    // The rest of the queue is untouched and still converts.
    assert.equal((await card(page, 'good.txt')).state, 'ready');
    await chooseTarget(page, 'good.txt', 'base64');
    await convert(page, 'good.txt');
    const d = await save(page, ctx, 'good.txt', 'good.b64');
    assert.equal(Buffer.from(d.bytes.toString('latin1'), 'base64').toString(), TEXT);

    // Trying again fails the same way, without breaking anything.
    await page.click({ card: BROKEN_NAME, sel: tid('retry') });
    await waitForCard(page, BROKEN_NAME, 'failed');
    assert.equal(await page.text({ card: BROKEN_NAME, sel: '.error-title' }), statusTitle(expected.status));

    // "Change format" opens the picker for that file; dismissing it changes nothing.
    await page.click({ card: BROKEN_NAME, sel: tid('change-format') });
    await page.waitForSelector('dialog[data-testid="picker-dialog"][open]');
    await page.click(tid('picker-close'));
    await page.waitForGone('dialog[data-testid="picker-dialog"][open]');

    // The failed file can be removed; the finished one stays.
    await page.click({ card: BROKEN_NAME, sel: tid('remove') });
    await page.waitFor(() => document.querySelectorAll('[data-testid="file-card"]').length === 1);
    assert.deepEqual((await cards(page)).map((c) => [c.name, c.state]), [['good.txt', 'done']]);
    await files.remove();
  });

  e2e('an empty file is handled without errors', async ({ ctx }) => {
    const files = await tempDir();
    const empty = await files.file('empty.dat', Buffer.alloc(0));
    const page = await openApp(ctx, server());
    await chooseFiles(page, [empty]);
    await waitForReady(page, ['empty.dat']);
    await chooseTarget(page, 'empty.dat', 'checksums');
    await convert(page, 'empty.dat');
    const ext = core.convert(new Uint8Array(0), { from: 'bin', to: 'checksums' }).extension;
    const d = await save(page, ctx, 'empty.dat', `empty.${ext}`);
    assert.match(d.bytes.toString('utf8'), /e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855/);
    await files.remove();
  });
});

// Cancelling: one long conversion, all of them at once, and converting
// again afterwards (the same file to completion, and another file).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { card, chooseFiles, chooseTarget, convert, openApp, outputName, save, tid, waitForCard, waitForReady } from './lib/app.mjs';
import { TEXT, sha256, tempDir } from './lib/fixtures.mjs';
import { core, e2e, server, suite } from './lib/harness.mjs';

/**
 * Size of the "long" inputs. Large enough that reading and hashing take
 * seconds, so the cancel always lands while the conversion runs.
 */
const LONG_MB = Number(process.env.E2E_LONG_MB || 192);

function longInput(seed) {
  const data = Buffer.alloc(LONG_MB * 1024 * 1024);
  for (let i = 0; i < data.length; i += 4096) data.writeUInt32LE((i * 2654435761 + seed) >>> 0, i);
  return data;
}

/** Waits for a condition on the Node side (driver state such as attached targets). */
async function until(fn, what, timeout = 10_000) {
  const deadline = Date.now() + timeout;
  while (!fn()) {
    if (Date.now() > deadline) assert.fail(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, 25));
  }
}

const checksumsExt = core?.convert(new Uint8Array([1]), { from: 'bin', to: 'checksums' }).extension;

suite('cancel', () => {
  e2e('cancel a running conversion, then convert it again to completion', async ({ ctx }) => {
    const files = await tempDir();
    const data = longInput(1);
    const long = await files.file('long.bin', data);
    const quick = await files.file('quick.txt', TEXT);
    const page = await openApp(ctx, server());
    await chooseFiles(page, [long, quick]);
    await waitForReady(page, ['long.bin', 'quick.txt']);
    await chooseTarget(page, 'long.bin', 'checksums');

    const workersBefore = ctx.started.worker ?? 0;
    await page.click({ card: 'long.bin', sel: tid('convert') });
    await waitForCard(page, 'long.bin', 'converting');
    await page.waitForSelector(tid('cancel-all')); // offered while busy
    const t0 = Date.now();
    await page.click({ card: 'long.bin', sel: tid('cancel') });
    await waitForCard(page, 'long.bin', 'cancelled', { timeout: 5_000 });
    assert.ok(Date.now() - t0 < 3_000, `cancelling is immediate (${Date.now() - t0} ms)`);
    assert.match(await page.text({ card: 'long.bin', sel: '.card-status-muted' }), /Cancelled/);
    // A running conversion is stopped by replacing its worker.
    await until(() => (ctx.started.worker ?? 0) > workersBefore, 'a fresh worker to replace the cancelled one');
    await page.waitFor(() => document.querySelector('[data-testid="cancel-all"]').hidden);

    // The other file is unaffected and converts.
    await chooseTarget(page, 'quick.txt', 'base64');
    await convert(page, 'quick.txt');
    const q = await save(page, ctx, 'quick.txt', 'quick.b64');
    assert.equal(Buffer.from(q.bytes.toString('latin1'), 'base64').toString(), TEXT);

    // Converting the cancelled file again runs to the end with the right result.
    await convert(page, 'long.bin', { timeout: 120_000 });
    const report = (await save(page, ctx, 'long.bin', outputName('long.bin', checksumsExt))).bytes.toString('utf8');
    assert.ok(report.includes(sha256(data)), 'SHA-256 of the whole file');
    await files.remove();
  }, { timeout: 180_000 });

  e2e('Cancel all stops every running and waiting conversion; converting works afterwards', async ({ ctx }) => {
    const files = await tempDir();
    const names = ['long-a.bin', 'long-b.bin', 'long-c.bin', 'long-d.bin'];
    const paths = [];
    for (const [i, n] of names.entries()) paths.push(await files.file(n, longInput(i + 10)));
    const quick = await files.file('after.txt', TEXT);
    const page = await openApp(ctx, server());
    await chooseFiles(page, [...paths, quick]);
    await waitForReady(page, [...names, 'after.txt']);
    // All long files to checksums through the group control.
    const source = (await card(page, names[0])).badge === 'FILE' ? 'bin' : (await card(page, names[0])).badge.toLowerCase();
    await page.click({ within: tid(`bulk-${source}`), sel: tid('bulk-target') });
    await page.waitForSelector('dialog[data-testid="picker-dialog"][open]');
    await page.focus(tid('picker-search'));
    await page.type('checksums');
    await page.click(`dialog[open] ${tid('target-option-checksums')}`);
    await page.waitForGone('dialog[data-testid="picker-dialog"][open]');
    for (const n of names) await page.waitFor((name) => [...document.querySelectorAll('[data-testid="file-card"]')].find((li) => li.querySelector('.file-name')?.textContent === name)?.querySelector('[data-testid="target-picker"] .badge')?.textContent.trim() === 'CHECKSUMS', [n]);
    // Leave the text file out of "Convert all": remove it, add it back later.
    await page.click({ card: 'after.txt', sel: tid('remove') });
    await page.waitFor(() => document.querySelectorAll('[data-testid="file-card"]').length === 4);

    await page.click(tid('convert-all'));
    for (const n of names) await waitForCard(page, n, ['queued', 'converting']);
    await waitForCard(page, names[0], 'converting');
    await page.waitForSelector('#overall'); // overall progress is shown
    await page.click(tid('cancel-all'));
    for (const n of names) await waitForCard(page, n, 'cancelled', { timeout: 5_000 });
    await page.waitFor(() => document.getElementById('overall').hidden);
    const chrome = await page.evaluate(() => ({
      cancelAll: document.querySelector('[data-testid="cancel-all"]').hidden,
      overall: document.getElementById('overall').hidden,
      convertAll: document.querySelector('[data-testid="convert-all"]').textContent.trim(),
    }));
    assert.deepEqual(chrome, { cancelAll: true, overall: true, convertAll: `Convert ${names.length} files` });

    // A new file converts normally afterwards.
    await chooseFiles(page, [quick]);
    await waitForReady(page, ['after.txt']);
    await chooseTarget(page, 'after.txt', 'hex');
    await convert(page, 'after.txt');
    const hex = await save(page, ctx, 'after.txt', 'after.hex');
    assert.equal(Buffer.from(hex.bytes.toString('latin1').replace(/\s+/g, ''), 'hex').toString(), TEXT);
    await files.remove();
  }, { timeout: 180_000 });
});

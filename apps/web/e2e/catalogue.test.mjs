// Catalogue-conditional conversions: each runs only when the core in this
// build can do it, and is reported as "skipped: format not in catalogue"
// otherwise. Outputs are verified independently in Node.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { chooseFiles, chooseTarget, convert, openAdvanced, openApp, outputName, save, tid, waitForCard, waitForReady } from './lib/app.mjs';
import { TEXT, isJpeg, makePng, pseudoRandom, readZip, tempDir } from './lib/fixtures.mjs';
import { appModule, core, e2e, needsFormats, server, suite } from './lib/harness.mjs';

const { buildControls, optionsFor } = await appModule('model/options.js');
const { DEFAULT_SETTINGS } = await appModule('model/settings.js');

const format = (id) => core?.formats().find((f) => f.id === id) ?? null;
const canConvert = (from, to) => Boolean(format(from)?.readable) && (core?.targets(from) ?? []).some((t) => t.id === to);
const jpegId = ['jpg', 'jpeg'].find((id) => canConvert('png', id)) ?? null;
const zipFrom = ['bin', 'txt'].filter((s) => (core?.targets(s) ?? []).some((t) => t.id === 'zip'));

/** A one-second 440 Hz mono 16-bit PCM WAV. */
function makeWav(rate = 22050) {
  const samples = rate;
  const data = Buffer.alloc(samples * 2);
  for (let i = 0; i < samples; i += 1) data.writeInt16LE(Math.round(Math.sin((2 * Math.PI * 440 * i) / rate) * 12000), i * 2);
  const h = Buffer.alloc(44);
  h.write('RIFF', 0, 'latin1');
  h.writeUInt32LE(36 + data.length, 4);
  h.write('WAVEfmt ', 8, 'latin1');
  h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20);
  h.writeUInt16LE(1, 22);
  h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * 2, 28);
  h.writeUInt16LE(2, 32);
  h.writeUInt16LE(16, 34);
  h.write('data', 36, 'latin1');
  h.writeUInt32LE(data.length, 40);
  return Buffer.concat([h, data]);
}

const audioTarget = (() => {
  if (!format('wav')?.readable) return null;
  return (core?.targets('wav') ?? []).find((t) => t.category === 'audio' && !t.compress && format(t.id)?.writable) ?? null;
})();

const MAGIC = { flac: 'fLaC', ogg: 'OggS', opus: 'OggS', wav: 'RIFF', aiff: 'FORM', mp3: null };

/** Moves a range control (by label) to `value` (its min or max) with Home/End. */
async function slideTo(page, name, label, value, key) {
  await openAdvanced(page, name);
  const id = await page.evaluate((n, l) => {
    const li = [...document.querySelectorAll('[data-testid="file-card"]')].find((el) => el.querySelector('.file-name')?.textContent === n);
    const lab = li && [...li.querySelectorAll('label.field-label')].find((x) => x.textContent === l);
    return lab ? lab.htmlFor : null;
  }, name, label);
  assert.ok(id, `a control labelled ${label}`);
  await page.focus(`#${id}`);
  await page.press(key);
  // The card re-renders with the new value (new element ids): look it up by label again.
  await page.waitFor(
    (n, l, v) => {
      const li = [...document.querySelectorAll('[data-testid="file-card"]')].find((el) => el.querySelector('.file-name')?.textContent === n);
      const lab = li && [...li.querySelectorAll('label.field-label')].find((x) => x.textContent === l);
      return lab && document.getElementById(lab.htmlFor)?.value === String(v);
    },
    [name, label, value],
    { message: `${label} to be ${value}` },
  );
}

suite('catalogue-dependent conversions', () => {
  e2e('PNG to JPEG: JPEG bytes, smaller at lower quality', async ({ ctx }) => {
    const png = makePng();
    const files = await tempDir();
    const page = await openApp(ctx, server());
    await chooseFiles(page, [await files.file('photo.png', png)]);
    await waitForReady(page, ['photo.png']);
    await chooseTarget(page, 'photo.png', jpegId);
    const schema = core.optionsSchema('png', jpegId);
    const quality = schema.find((o) => o.key === 'quality');
    const name = outputName('photo.png', core.convert(new Uint8Array(png), { from: 'png', to: jpegId }).extension);
    const sizes = {};
    for (const level of ['high', 'low']) {
      if (quality) await slideTo(page, 'photo.png', quality.label, level === 'high' ? quality.max : quality.min, level === 'high' ? 'End' : 'Home');
      else if (schema.some((o) => o.key === 'preset')) await page.click({ card: 'photo.png', sel: tid(level === 'high' ? 'preset-high' : 'preset-small') });
      await convert(page, 'photo.png', { timeout: 60_000 });
      const d = await save(page, ctx, 'photo.png', name);
      assert.ok(isJpeg(d.bytes), `${level}: starts with the JPEG magic and ends with EOI`);
      sizes[level] = d.bytes.length;
      await page.click({ card: 'photo.png', sel: tid('again') });
      await waitForCard(page, 'photo.png', 'ready');
    }
    assert.ok(sizes.low < sizes.high, `lower quality is smaller (${sizes.low} < ${sizes.high})`);
    await files.remove();
  }, { skip: needsFormats(jpegId, 'png -> jpg') });

  e2e('any file to ZIP: a valid archive holding the original bytes', async ({ ctx }) => {
    const input = pseudoRandom(150_000, 99);
    const files = await tempDir();
    const page = await openApp(ctx, server());
    await chooseFiles(page, [await files.file('payload.bin', input), await files.file('notes.txt', TEXT)]);
    await waitForReady(page, ['payload.bin', 'notes.txt']);
    for (const [name, data] of [['payload.bin', input], ['notes.txt', Buffer.from(TEXT)]]) {
      await chooseTarget(page, name, 'zip');
      await convert(page, name, { timeout: 60_000 });
      const ext = core.convert(new Uint8Array(data), { to: 'zip', filename: name }).extension;
      const d = await save(page, ctx, name, outputName(name, ext));
      const entries = readZip(d.bytes);
      assert.equal(entries.length, 1, `${name}: one entry`);
      assert.ok(entries[0].name.endsWith(name), `${name}: entry named after the file (${entries[0].name})`);
      assert.ok(entries[0].data.equals(data), `${name}: the entry holds the exact bytes`);
    }
    await files.remove();
  }, { skip: needsFormats(zipFrom.length === 2, 'bin/txt -> zip') });

  e2e('WAV to another audio format: the browser output equals the core in Node', async ({ ctx }) => {
    const wav = makeWav();
    const files = await tempDir();
    const page = await openApp(ctx, server());
    await chooseFiles(page, [await files.file('tone.wav', wav)]);
    await waitForReady(page, ['tone.wav']);
    await chooseTarget(page, 'tone.wav', audioTarget.id);
    await convert(page, 'tone.wav', { timeout: 60_000 });
    // The app sends what the user's settings say (default quality preset
    // "balanced", metadata removed), not the core's own defaults: ask the core
    // in Node for exactly that.
    const options = optionsFor(buildControls(core.optionsSchema('wav', audioTarget.id)), {}, DEFAULT_SETTINGS);
    const expected = core.convert(new Uint8Array(wav), { from: 'wav', to: audioTarget.id, filename: 'tone.wav', options });
    assert.equal(expected.status, 0, expected.message);
    const d = await save(page, ctx, 'tone.wav', outputName('tone.wav', expected.extension));
    const magic = MAGIC[audioTarget.id];
    if (magic) assert.equal(d.bytes.subarray(0, 4).toString('latin1'), magic);
    assert.ok(d.bytes.length > 0);
    assert.ok(d.bytes.equals(Buffer.from(expected.data)), 'identical to the core run in Node');
    await files.remove();
  }, { skip: needsFormats(audioTarget, 'wav -> audio') });
});

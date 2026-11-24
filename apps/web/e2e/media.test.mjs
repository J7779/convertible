// Real media and document conversions in the browser, verified independently:
// images and audio decoded by the browser's own decoders, GIF frames, PDF
// pages, spreadsheet cells and JSON values parsed in Node, the size limit.
// Every test is catalogue-conditional: targets are found by category and MIME
// type in the live catalogue, never by position, and a missing capability is
// reported as "skipped: format not in catalogue".
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { deflateSync, inflateSync } from 'node:zlib';
import { chooseFiles, chooseTarget, convert, openApp, outputName, save, tid, waitForCard, waitForReady } from './lib/app.mjs';
import { crc32, makePng, tempDir } from './lib/fixtures.mjs';
import { appModule, core, e2e, needsFormats, server, suite } from './lib/harness.mjs';

const { buildControls, optionsFor } = await appModule('model/options.js');
const { DEFAULT_SETTINGS } = await appModule('model/settings.js');

const format = (id) => core?.formats().find((f) => f.id === id) ?? null;
const targetsOf = (id) => (format(id)?.readable ? core.targets(id) : []);
const canConvert = (from, to) => targetsOf(from).some((t) => t.id === to);
/** What the app sends for a conversion with default settings. */
const appOptions = (from, to) => optionsFor(buildControls(core.optionsSchema(from, to)), {}, DEFAULT_SETTINGS);
const expectedName = (input, data, from, to) => outputName(input, core.convert(new Uint8Array(data), { from, to, filename: input, options: appOptions(from, to) }).extension);

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

/** A one-second 440 Hz mono 16-bit PCM WAV. */
function makeWav(rate = 22050, seconds = 1) {
  const samples = Math.round(rate * seconds);
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

/** A YUV4MPEG2 clip: `frames` 4:2:0 frames of a moving gradient. */
function makeY4m(width = 32, height = 24, frames = 6) {
  const parts = [Buffer.from(`YUV4MPEG2 W${width} H${height} F10:1 Ip A1:1 C420jpeg\n`, 'latin1')];
  for (let f = 0; f < frames; f += 1) {
    const y = Buffer.alloc(width * height);
    for (let j = 0; j < height; j += 1) for (let i = 0; i < width; i += 1) y[j * width + i] = (i * 8 + f * 30 + j * 4) & 0xff;
    const c = Buffer.alloc((width / 2) * (height / 2), 128 + f * 8);
    parts.push(Buffer.from('FRAME\n', 'latin1'), y, c, Buffer.alloc(c.length, 128 - f * 8));
  }
  return Buffer.concat(parts);
}

/** A PNG whose header claims 60000 x 60000 pixels (3.6 gigapixels). */
function makeHugePng() {
  const chunk = (type, data) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, 'latin1'), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(60000, 0);
  ihdr.writeUInt32BE(60000, 4);
  ihdr[8] = 8;
  ihdr[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(Buffer.alloc(4096))), chunk('IEND', Buffer.alloc(0))]);
}

const MARKDOWN = `# Convertible test document\n\nThis document has **bold** text, a list and enough paragraphs for two pages.\n\n- one\n- two\n- three\n\n${'A paragraph of ordinary text that wraps over several lines. '.repeat(12)}\n\n`.repeat(8);
const CSV_ROWS = [
  ['name', 'qty', 'price'],
  ['Apple', '3', '1.25'],
  ['Pear', '10', '0.5'],
  ['Grüße, "quoted"', '7', '2'],
];
const toCsv = (rows) => `${rows.map((r) => r.map((c) => (/[",\n]/.test(c) ? `"${c.replaceAll('"', '""')}"` : c)).join(',')).join('\n')}\n`;
const parseCsv = (text) => {
  const rows = [];
  let row = [];
  let cell = '';
  let quoted = false;
  for (let i = 0; i < text.length; i += 1) {
    const ch = text[i];
    if (quoted) {
      if (ch === '"' && text[i + 1] === '"') {
        cell += '"';
        i += 1;
      } else if (ch === '"') quoted = false;
      else cell += ch;
    } else if (ch === '"') quoted = true;
    else if (ch === ',') {
      row.push(cell);
      cell = '';
    } else if (ch === '\n' || ch === '\r') {
      if (ch === '\r' && text[i + 1] === '\n') i += 1;
      row.push(cell);
      rows.push(row);
      row = [];
      cell = '';
    } else cell += ch;
  }
  if (cell || row.length) rows.push([...row, cell]);
  return rows;
};

// ---------------------------------------------------------------------------
// Independent checks
// ---------------------------------------------------------------------------

/** GIF: logical screen size and the number of image frames, by walking the blocks. */
function readGif(buf) {
  assert.match(buf.subarray(0, 6).toString('latin1'), /^GIF8[79]a$/);
  const width = buf.readUInt16LE(6);
  const height = buf.readUInt16LE(8);
  let p = 13;
  if (buf[10] & 0x80) p += 3 * (1 << ((buf[10] & 7) + 1));
  const skipBlocks = () => {
    while (buf[p] !== 0) p += buf[p] + 1;
    p += 1;
  };
  let frames = 0;
  for (;;) {
    const b = buf[p];
    if (b === 0x3b) break;
    if (b === 0x21) {
      p += 2;
      skipBlocks();
    } else if (b === 0x2c) {
      frames += 1;
      const flags = buf[p + 9];
      p += 10;
      if (flags & 0x80) p += 3 * (1 << ((flags & 7) + 1));
      p += 1; // LZW minimum code size
      skipBlocks();
    } else throw new Error(`unexpected GIF block 0x${b?.toString(16)} at ${p}`);
  }
  return { width, height, frames };
}

/** PDF page objects, counted in the file and inside compressed (object) streams. */
function pdfPages(buf) {
  const text = buf.toString('latin1');
  let all = text;
  for (const m of text.matchAll(/stream\r?\n/g)) {
    const start = m.index + m[0].length;
    const end = text.indexOf('endstream', start);
    if (end < 0) continue;
    try {
      all += inflateSync(buf.subarray(start, end)).toString('latin1');
    } catch {
      /* not a Flate stream (images, fonts) */
    }
  }
  return [...all.matchAll(/\/Type\s*\/Page(?![a-zA-Z])/g)].length;
}

const BROWSER_IMAGE_TYPES = ['image/webp', 'image/avif', 'image/gif', 'image/bmp', 'image/png'];
const BROWSER_AUDIO_TYPES = ['audio/ogg; codecs=opus', 'audio/flac', 'audio/mpeg', 'audio/ogg', 'audio/wav', 'audio/aac', 'audio/mp4'];

/** Decodes bytes with the browser's own image decoder: {width, height, mean RGB}. */
function decodeImageInPage(page, bytes, mime) {
  return page.evaluate(
    async (b64, type) => {
      const data = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
      const bitmap = await createImageBitmap(new Blob([data], { type }));
      const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
      const g = canvas.getContext('2d');
      g.drawImage(bitmap, 0, 0);
      const px = g.getImageData(0, 0, bitmap.width, bitmap.height).data;
      const mean = [0, 0, 0];
      for (let i = 0; i < px.length; i += 4) for (let c = 0; c < 3; c += 1) mean[c] += px[i + c];
      return { width: bitmap.width, height: bitmap.height, mean: mean.map((v) => v / (px.length / 4)) };
    },
    Buffer.from(bytes).toString('base64'),
    mime,
  );
}

/** Decodes audio with the browser's own decoders: {duration, rms}. */
function decodeAudioInPage(page, bytes) {
  return page.evaluate(async (b64) => {
    const data = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
    const ctx = new OfflineAudioContext(1, 48000, 48000);
    const audio = await ctx.decodeAudioData(data.buffer);
    const ch = audio.getChannelData(0);
    let sum = 0;
    for (const v of ch) sum += v * v;
    return { duration: audio.duration, rms: Math.sqrt(sum / ch.length) };
  }, Buffer.from(bytes).toString('base64'));
}

/** Candidate targets of `source` in `category` whose MIME type the page says it can decode. */
async function decodableTargets(page, source, category, kinds) {
  const offered = targetsOf(source).filter((t) => t.category === category && !t.compress && t.id !== source && format(t.id)?.writable);
  return page.evaluate(
    (list, types, isAudio) => {
      const probe = isAudio ? document.createElement('audio') : null;
      return list.filter((t) => types.includes(t.mime) && (!isAudio || probe.canPlayType(t.mime) !== ''));
    },
    offered.map((t) => ({ id: t.id, mime: t.mime })),
    kinds,
    category === 'audio',
  );
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const imageOk = format('png')?.readable && targetsOf('png').some((t) => t.category === 'image' && BROWSER_IMAGE_TYPES.includes(t.mime) && t.id !== 'png');
const audioOk = format('wav')?.readable && targetsOf('wav').some((t) => t.category === 'audio' && BROWSER_AUDIO_TYPES.includes(t.mime) && t.id !== 'wav');
const videoTarget = format('y4m')?.readable ? (canConvert('y4m', 'gif') ? 'gif' : canConvert('y4m', 'png') ? 'png' : null) : null;
const mdSource = ['md', 'markdown'].find((id) => canConvert(id, 'pdf')) ?? null;
const csvOk = canConvert('csv', 'xlsx') && canConvert('xlsx', 'csv');
const jsonOk = canConvert('csv', 'json');

suite('media and document conversions', () => {
  e2e('PNG to another image format: the browser decodes the same picture', async ({ ctx }) => {
    const png = makePng(160, 120);
    const files = await tempDir();
    const page = await openApp(ctx, server());
    const [target] = await decodableTargets(page, 'png', 'image', BROWSER_IMAGE_TYPES);
    assert.ok(target, 'an image target the browser can decode');
    await chooseFiles(page, [await files.file('picture.png', png)]);
    await waitForReady(page, ['picture.png']);
    await chooseTarget(page, 'picture.png', target.id);
    await convert(page, 'picture.png', { timeout: 60_000 });
    const d = await save(page, ctx, 'picture.png', expectedName('picture.png', png, 'png', target.id));
    const original = await decodeImageInPage(page, png, 'image/png');
    const converted = await decodeImageInPage(page, d.bytes, target.mime);
    assert.deepEqual([converted.width, converted.height], [160, 120], `${target.id} keeps the size`);
    for (let c = 0; c < 3; c += 1) assert.ok(Math.abs(converted.mean[c] - original.mean[c]) < 12, `${target.id}: channel ${c} mean ${converted.mean[c]} vs ${original.mean[c]}`);
    await files.remove();
  }, { skip: needsFormats(imageOk, 'png -> a browser-decodable image format') });

  e2e('WAV to a compressed audio format: the browser decodes one second of tone', async ({ ctx }) => {
    const wav = makeWav();
    const files = await tempDir();
    const page = await openApp(ctx, server());
    const candidates = await decodableTargets(page, 'wav', 'audio', BROWSER_AUDIO_TYPES);
    const target = candidates.find((t) => t.mime !== 'audio/wav') ?? candidates[0];
    assert.ok(target, 'an audio target the browser can decode');
    await chooseFiles(page, [await files.file('tone.wav', wav)]);
    await waitForReady(page, ['tone.wav']);
    await chooseTarget(page, 'tone.wav', target.id);
    await convert(page, 'tone.wav', { timeout: 60_000 });
    const d = await save(page, ctx, 'tone.wav', expectedName('tone.wav', wav, 'wav', target.id));
    const { duration, rms } = await decodeAudioInPage(page, d.bytes);
    assert.ok(duration > 0.9 && duration < 1.2, `${target.id}: about one second (${duration.toFixed(3)} s)`);
    assert.ok(rms > 0.15 && rms < 0.4, `${target.id}: the tone survives (RMS ${rms.toFixed(3)}; a 12000/32768 sine is 0.26)`);
    await files.remove();
  }, { skip: needsFormats(audioOk, 'wav -> a browser-decodable audio format') });

  e2e('a short video to an animated GIF (or a still frame)', async ({ ctx }) => {
    const clip = makeY4m();
    const files = await tempDir();
    const page = await openApp(ctx, server());
    await chooseFiles(page, [await files.file('clip.y4m', clip)]);
    await waitForReady(page, ['clip.y4m']);
    await chooseTarget(page, 'clip.y4m', videoTarget);
    await convert(page, 'clip.y4m', { timeout: 60_000 });
    const d = await save(page, ctx, 'clip.y4m', expectedName('clip.y4m', clip, 'y4m', videoTarget));
    if (videoTarget === 'gif') {
      const gif = readGif(d.bytes);
      assert.deepEqual([gif.width, gif.height], [32, 24]);
      assert.ok(gif.frames >= 2, `animated: ${gif.frames} frames`);
    } else {
      const frame = await decodeImageInPage(page, d.bytes, 'image/png');
      assert.deepEqual([frame.width, frame.height], [32, 24]);
    }
    await files.remove();
  }, { skip: needsFormats(videoTarget, 'y4m -> gif or png') });

  e2e('Markdown to PDF: a real PDF with several pages', async ({ ctx }) => {
    const files = await tempDir();
    const page = await openApp(ctx, server());
    const name = `doc.${format(mdSource).extensions[0]}`;
    await chooseFiles(page, [await files.file(name, MARKDOWN)]);
    await waitForReady(page, [name]);
    await chooseTarget(page, name, 'pdf');
    await convert(page, name, { timeout: 60_000 });
    const d = await save(page, ctx, name, expectedName(name, Buffer.from(MARKDOWN), mdSource, 'pdf'));
    assert.equal(d.bytes.subarray(0, 5).toString('latin1'), '%PDF-');
    assert.match(d.bytes.subarray(-32).toString('latin1'), /%%EOF\s*$/);
    const pages = pdfPages(d.bytes);
    assert.ok(pages >= 2, `${pages} pages`);
    await files.remove();
  }, { skip: needsFormats(mdSource, 'markdown -> pdf') });

  e2e('CSV to XLSX and back, and CSV to JSON: every cell survives', async ({ ctx }) => {
    const csv = toCsv(CSV_ROWS);
    const files = await tempDir();
    const page = await openApp(ctx, server());
    await chooseFiles(page, [await files.file('table.csv', csv)]);
    await waitForReady(page, ['table.csv']);
    await chooseTarget(page, 'table.csv', 'xlsx');
    await convert(page, 'table.csv', { timeout: 60_000 });
    const xlsx = await save(page, ctx, 'table.csv', expectedName('table.csv', Buffer.from(csv), 'csv', 'xlsx'));
    assert.equal(xlsx.bytes.subarray(0, 2).toString('latin1'), 'PK', 'an OOXML package');
    // Back to CSV in the app: the same cells.
    await chooseFiles(page, [await files.file('table.xlsx', xlsx.bytes)]);
    await waitForReady(page, ['table.xlsx']);
    await chooseTarget(page, 'table.xlsx', 'csv');
    await convert(page, 'table.xlsx', { timeout: 60_000 });
    const back = await save(page, ctx, 'table.xlsx', expectedName('table.xlsx', xlsx.bytes, 'xlsx', 'csv'));
    assert.deepEqual(parseCsv(back.bytes.toString('utf8').replace(/^﻿/, '')), CSV_ROWS);
    if (jsonOk) {
      await page.click({ card: 'table.csv', sel: '[data-testid="again"]' });
      await waitForCard(page, 'table.csv', 'ready');
      await chooseTarget(page, 'table.csv', 'json');
      await convert(page, 'table.csv', { timeout: 60_000 });
      const json = await save(page, ctx, 'table.csv', expectedName('table.csv', Buffer.from(csv), 'csv', 'json'));
      const values = [];
      const walk = (v) => (v && typeof v === 'object' ? Object.values(v).forEach(walk) : values.push(String(v)));
      walk(JSON.parse(json.bytes.toString('utf8')));
      for (const row of CSV_ROWS.slice(1)) for (const cell of row) assert.ok(values.includes(cell) || values.includes(String(Number(cell))), `${cell} is in the JSON`);
    }
    await files.remove();
  }, { skip: needsFormats(csvOk, 'csv <-> xlsx') });

  e2e('a picture over the size limit fails with a friendly message; the queue survives', async ({ ctx }) => {
    const huge = makeHugePng();
    const files = await tempDir();
    const page = await openApp(ctx, server());
    await chooseFiles(page, [await files.file('huge.png', huge)]);
    await waitForReady(page, ['huge.png']);
    const target = targetsOf('png').find((t) => t.category === 'image' && !t.compress).id;
    await chooseTarget(page, 'huge.png', target);
    await page.click({ card: 'huge.png', sel: tid('convert') });
    assert.equal(await waitForCard(page, 'huge.png', ['done', 'failed'], { timeout: 60_000 }), 'failed');
    const text = await page.text({ card: 'huge.png', sel: tid('error') });
    assert.match(text, /too large/i, text);
    // The app keeps working: another file converts afterwards.
    await chooseFiles(page, [await files.file('small.png', makePng(32, 32))]);
    await waitForReady(page, ['small.png']);
    await convert(page, 'small.png', { timeout: 60_000 });
    await files.remove();
  }, { skip: needsFormats(format('png')?.readable && targetsOf('png').some((t) => t.category === 'image'), 'png -> image') });
});

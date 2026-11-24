// Binding tests: REAL conversions through the real WebAssembly core.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { before, describe, test } from 'node:test';
import { importSrc, loadCore, text, utf8 } from './helpers.mjs';

let core;
let Status;
let CoreError;

before(async () => {
  core = await loadCore();
  ({ Status, CoreError } = await importSrc('js/core/binding.js'));
});

describe('library and catalogue', () => {
  test('version is a semantic version', () => {
    assert.match(core.version(), /^\d+\.\d+\.\d+/);
  });

  test('formats are well formed and most popular first', () => {
    const formats = core.formats();
    assert.ok(formats.length > 10);
    const ids = new Set();
    for (const f of formats) {
      // Canonical ids are lower case; the C ABI does not restrict them further
      // (for example "ds_store" or "tar.gz"), so neither does the app.
      assert.match(f.id, /^[a-z0-9][a-z0-9._+-]*$/);
      assert.ok(!ids.has(f.id), `duplicate ${f.id}`);
      ids.add(f.id);
      assert.equal(typeof f.name, 'string');
      assert.equal(typeof f.category, 'string');
      assert.ok(Array.isArray(f.extensions));
      assert.equal(typeof f.readable, 'boolean');
      assert.equal(typeof f.writable, 'boolean');
    }
    for (let i = 1; i < formats.length; i += 1) assert.ok(formats[i - 1].popularity >= formats[i].popularity);
    for (const id of ['txt', 'bin', 'base64', 'hex', 'checksums']) assert.ok(ids.has(id), id);
  });

  test('every readable format lists targets, and every target has a schema', () => {
    for (const f of core.formats().filter((x) => x.readable)) {
      const targets = core.targets(f.id);
      assert.ok(Array.isArray(targets));
      for (const t of targets) {
        assert.equal(typeof t.lossy, 'boolean');
        assert.equal(typeof t.compress, 'boolean');
        if (t.compress) assert.equal(t.id, f.id);
        const schema = core.optionsSchema(f.id, t.id);
        assert.ok(Array.isArray(schema));
        for (const spec of schema) assert.ok(['int', 'float', 'bool', 'enum', 'string'].includes(spec.type), spec.type);
      }
    }
  });

  test('any file converts to the universal encodings', () => {
    const targets = core.targets('bin').map((t) => t.id);
    for (const id of ['base64', 'hex', 'checksums']) assert.ok(targets.includes(id));
    assert.deepEqual(core.targets('definitely-not-a-format'), []);
  });

  test('detection', () => {
    assert.equal(core.detect(new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13]), 'x.png'), 'png');
    assert.equal(core.detect(utf8('plain words here'), 'notes.txt'), 'txt');
    assert.equal(core.detect(new Uint8Array([0, 1, 2, 200]), null), null);
  });
});

describe('real conversions', () => {
  test('text -> Base64 -> back is lossless', () => {
    const original = 'Hello, world! Grüße, 你好 \u{1F600}\n';
    const encoded = core.convert(utf8(original), { to: 'base64', filename: 'hello.txt' });
    assert.equal(encoded.status, Status.OK);
    assert.equal(encoded.format, 'base64');
    assert.equal(encoded.extension, 'b64');
    assert.equal(text(encoded.data).trim(), Buffer.from(original).toString('base64'));
    const decoded = core.convert(encoded.data, { from: 'base64', to: 'bin' });
    assert.equal(decoded.status, Status.OK);
    assert.equal(text(decoded.data), original);
  });

  test('known Base64 vector', () => {
    const r = core.convert(utf8('Hello, world!'), { to: 'base64' });
    assert.equal(text(r.data), 'SGVsbG8sIHdvcmxkIQ==\n');
    const unwrapped = core.convert(utf8('a'.repeat(100)), { to: 'base64', options: { line_width: 0 } });
    assert.ok(!text(unwrapped.data).trim().includes('\n'));
  });

  test('every byte value survives hex and back', () => {
    const all = Uint8Array.from({ length: 256 }, (_, i) => i);
    const hex = core.convert(all, { from: 'bin', to: 'hex' });
    assert.equal(hex.status, Status.OK);
    assert.equal(text(hex.data).replace(/\s+/g, '').toLowerCase(), Buffer.from(all).toString('hex'));
    const back = core.convert(hex.data, { from: 'hex', to: 'bin' });
    assert.equal(back.status, Status.OK);
    assert.deepEqual([...back.data], [...all]);
  });

  test('checksums match the standard test vectors', () => {
    const r = core.convert(utf8('abc'), { to: 'checksums' });
    assert.equal(r.status, Status.OK);
    const report = text(r.data);
    assert.match(report, /352441c2/); // CRC-32
    assert.match(report, /900150983cd24fb0d6963f7d28e17f72/); // MD5
    assert.match(report, /a9993e364706816aba3e25717850c26c9cd0d89d/); // SHA-1
    assert.match(report, /ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad/); // SHA-256
  });

  test('checksums of a larger buffer agree with Node crypto', () => {
    const data = Uint8Array.from({ length: 300_000 }, (_, i) => (i * 7919) & 0xff);
    const r = core.convert(data, { from: 'bin', to: 'checksums' });
    const report = text(r.data);
    assert.ok(report.includes(createHash('sha256').update(data).digest('hex')));
    assert.ok(report.includes(createHash('md5').update(data).digest('hex')));
  });

  test('text re-encoding: UTF-8 -> UTF-16LE with BOM, and Windows-1252 -> UTF-8', () => {
    const r = core.convert(utf8('héllo'), { from: 'txt', to: 'txt', options: { encoding: 'utf-16le', bom: true } });
    assert.equal(r.status, Status.OK);
    assert.deepEqual([...r.data], [0xff, 0xfe, 0x68, 0, 0xe9, 0, 0x6c, 0, 0x6c, 0, 0x6f, 0]);
    const latin = new Uint8Array([0x63, 0x61, 0x66, 0xe9, 0x20, 0x80]); // "café €" in Windows-1252
    const u = core.convert(latin, { from: 'txt', to: 'txt', options: { source_encoding: 'windows-1252', encoding: 'utf-8' } });
    assert.equal(u.status, Status.OK);
    assert.equal(text(u.data), 'café €');
  });

  test('a filled heap buffer converts without an extra copy', () => {
    const input = utf8('stream me');
    const buffer = core.allocate(input.length);
    buffer.view().set(input);
    const r = core.convert(buffer, { to: 'hex' , from: 'bin' });
    buffer.free();
    assert.equal(text(r.data).replace(/\s+/g, ''), Buffer.from(input).toString('hex'));
  });

  test('empty input', () => {
    const r = core.convert(new Uint8Array(0), { from: 'bin', to: 'checksums' });
    assert.equal(r.status, Status.OK);
    assert.match(text(r.data), /e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855/);
  });
});

describe('errors', () => {
  test('statuses are mapped and messages are human readable', () => {
    const unknown = core.convert(utf8('x'), { to: 'nope' });
    assert.equal(unknown.status, Status.UNKNOWN_FORMAT);
    assert.equal(unknown.ok, false);
    assert.equal(unknown.data, null);
    assert.ok(unknown.message.length > 5);

    assert.equal(core.convert(utf8('@@@@!!'), { from: 'base64', to: 'bin' }).status, Status.DECODE);
    assert.equal(core.convert(utf8('abc'), { to: 'base64', options: { line_width: 'wide' } }).status, Status.INVALID_ARGUMENT);
    assert.equal(core.convert(utf8('abc'), { to: 'base64', options: '{not json' }).status, Status.INVALID_ARGUMENT);
    assert.equal(core.convert(utf8('abc'), { from: 'txt', to: 'definitely-not-a-format' }).status, Status.UNKNOWN_FORMAT);
    // Undetectable input with no source format.
    assert.equal(core.convert(new Uint8Array([0, 1, 2, 200]), { to: 'hex' }).status, Status.UNKNOWN_FORMAT);
  });

  test('a writable format that is not reachable is UNSUPPORTED_CONVERSION', () => {
    const formats = core.formats();
    const reachable = new Set(core.targets('bin').map((t) => t.id));
    const unreachable = formats.find((f) => f.writable && !reachable.has(f.id) && f.id !== 'bin');
    if (!unreachable) return; // every writable format accepts raw bytes in this build
    assert.equal(core.convert(utf8('abc'), { from: 'bin', to: unreachable.id }).status, Status.UNSUPPORTED_CONVERSION);
  });

  test('the binding rejects bad sizes and re-entrant use', () => {
    assert.throws(() => core.allocate(-1), CoreError);
    let inner = null;
    const outer = core.convert(utf8('a'), {
      to: 'hex',
      from: 'bin',
      onProgress: () => {
        try {
          core.convert(utf8('b'), { to: 'hex', from: 'bin' });
        } catch (error) {
          inner = error;
        }
      },
    });
    assert.ok(inner instanceof CoreError);
    assert.equal(outer.status, Status.OK);
  });
});

describe('progress and cancellation', () => {
  test('progress is reported in order and ends at 1', () => {
    const seen = [];
    const r = core.convert(utf8('abc'.repeat(100_000)), { to: 'base64', onProgress: (f, stage) => void seen.push([f, stage]) });
    assert.equal(r.status, Status.OK);
    assert.ok(seen.length >= 2);
    for (let i = 1; i < seen.length; i += 1) assert.ok(seen[i][1] !== undefined && seen[i][0] >= seen[i - 1][0]);
    assert.equal(seen.at(-1)[0], 1);
    assert.ok(seen.every(([, s]) => typeof s === 'string' && s.length > 0));
  });

  test('returning true from the callback cancels', () => {
    let calls = 0;
    const r = core.convert(utf8('abc'), {
      to: 'base64',
      onProgress: () => {
        calls += 1;
        return true;
      },
    });
    assert.equal(r.status, Status.CANCELLED);
    assert.equal(calls, 1);
    assert.equal(r.data, null);
    // The instance is still usable afterwards.
    assert.equal(core.convert(utf8('abc'), { to: 'base64' }).status, Status.OK);
  });

  test('cancelling at the writing stage', () => {
    const r = core.convert(utf8('xyz'), { to: 'hex', from: 'bin', onProgress: (f) => f >= 0.5 });
    assert.equal(r.status, Status.CANCELLED);
  });
});

describe('memory', () => {
  test('1000 conversions do not leak (results, strings and buffers are freed)', () => {
    const input = utf8('memory check '.repeat(2000));
    const round = () => {
      const r = core.convert(input, { to: 'base64', filename: 'm.txt', options: { line_width: 64 } });
      assert.equal(r.status, Status.OK);
      core.targets('txt');
      core.optionsSchema('txt', 'base64');
      core.detect(input.subarray(0, 64), 'm.txt');
      const bad = core.convert(input, { to: 'nope' });
      assert.equal(bad.status, Status.UNKNOWN_FORMAT);
    };
    for (let i = 0; i < 50; i += 1) round(); // warm up allocator pools
    const heapBefore = core.memoryBytes();
    const probe = core.allocate(1 << 20);
    const probeBefore = probe.ptr;
    probe.free();
    for (let i = 0; i < 1000; i += 1) round();
    const probe2 = core.allocate(1 << 20);
    const probeAfter = probe2.ptr;
    probe2.free();
    assert.equal(core.memoryBytes(), heapBefore, 'the heap grew');
    assert.equal(probeAfter, probeBefore, 'allocator state drifted');
  });
});

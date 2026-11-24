// Core loader tests: streaming download with byte progress, one compile,
// and the error kinds the start-up screen distinguishes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { test } from 'node:test';
import { DIST, importSrc, requireDist } from './helpers.mjs';

const { loadCoreModule, CoreLoadError } = await importSrc('js/core/loader.js');

/** A fetch that serves `bytes` in `chunk`-sized pieces. */
function servingFetch(bytes, { chunk = 1 << 20, headers = {} } = {}) {
  return async () =>
    new Response(
      new ReadableStream({
        start(controller) {
          for (let i = 0; i < bytes.length; i += chunk) controller.enqueue(bytes.subarray(i, Math.min(bytes.length, i + chunk)));
          controller.close();
        },
      }),
      { status: 200, headers: { 'Content-Type': 'application/wasm', ...headers } },
    );
}

test('the real core downloads with monotonic progress and compiles once', async () => {
  requireDist();
  const bytes = new Uint8Array(await readFile(path.join(DIST, 'wasm', 'convertible.wasm')));
  const seen = [];
  const module = await loadCoreModule({
    url: 'convertible.wasm',
    expectedBytes: bytes.length,
    fetchImpl: servingFetch(bytes),
    onProgress: (loaded, total) => seen.push([loaded, total]),
  });
  assert.ok(module instanceof WebAssembly.Module);
  assert.ok(WebAssembly.Module.exports(module).length >= 17, 'the C ABI is exported (names are minified)');
  assert.deepEqual(seen[0], [0, bytes.length]);
  assert.deepEqual(seen.at(-1), [bytes.length, bytes.length]);
  for (let i = 1; i < seen.length; i += 1) assert.ok(seen[i][0] >= seen[i - 1][0], 'never goes back');
  assert.ok(seen.length > 5, 'reported while downloading');
});

test('without a known size, a compressed response reports an unknown total', async () => {
  const tiny = new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]);
  const totals = [];
  await loadCoreModule({ url: 'x.wasm', fetchImpl: servingFetch(tiny, { headers: { 'Content-Encoding': 'gzip', 'Content-Length': '3' } }), onProgress: (l, t) => totals.push(t) });
  assert.equal(totals[0], 0, 'Content-Length of a compressed body is not the byte total');
});

test('failures are classified for the start-up screen', async () => {
  const kind = async (fetchImpl) => {
    try {
      await loadCoreModule({ url: 'x.wasm', fetchImpl });
      return 'ok';
    } catch (error) {
      assert.ok(error instanceof CoreLoadError, String(error));
      return error.kind;
    }
  };
  assert.equal(await kind(async () => { throw new TypeError('Failed to fetch'); }), 'network');
  assert.equal(await kind(async () => new Response('missing', { status: 404 })), 'network');
  assert.equal(await kind(servingFetch(new TextEncoder().encode('not WebAssembly at all'))), 'compile');
});

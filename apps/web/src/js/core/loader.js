// Convertible web app - downloads and compiles the WebAssembly core once, on
// the main thread, with byte progress. The compiled WebAssembly.Module is
// then handed to every worker (modules are structured-cloneable), so the
// ~30 MB binary is fetched and compiled once instead of once per worker.
//
// Compilation streams: bytes are compiled while they arrive
// (WebAssembly.compileStreaming over a counting stream) where the browser
// supports it, otherwise they are collected and compiled at the end.
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

export class CoreLoadError extends Error {
  /** @param {'offline'|'network'|'unsupported'|'compile'} kind */
  constructor(kind, message) {
    super(message);
    this.name = 'CoreLoadError';
    this.kind = kind;
  }
}

/** True when this browser can run the core at all. */
export function wasmSupported() {
  return typeof WebAssembly === 'object' && typeof WebAssembly.compile === 'function';
}

/**
 * A ReadableStream that passes `body` through and reports the running byte count.
 */
function countingStream(body, onBytes) {
  const reader = body.getReader();
  let loaded = 0;
  return new ReadableStream({
    async pull(controller) {
      const { done, value } = await reader.read();
      if (done) {
        controller.close();
        return;
      }
      loaded += value.byteLength;
      onBytes(loaded);
      controller.enqueue(value);
    },
    cancel(reason) {
      return reader.cancel(reason);
    },
  });
}

/**
 * Fetches and compiles the core.
 * @param {object} o
 * @param {string|URL} o.url           convertible.wasm
 * @param {number} [o.expectedBytes]   uncompressed size from the build (Content-Length
 *                                     is the compressed size when the server compresses)
 * @param {(loaded:number, total:number) => void} [o.onProgress]
 * @param {typeof fetch} [o.fetchImpl]
 * @returns {Promise<WebAssembly.Module>}
 */
export async function loadCoreModule({ url, expectedBytes = 0, onProgress = () => {}, fetchImpl = fetch }) {
  if (!wasmSupported()) throw new CoreLoadError('unsupported', 'This browser cannot run WebAssembly.');
  let response;
  try {
    response = await fetchImpl(url, { credentials: 'same-origin' });
  } catch {
    const offline = typeof navigator !== 'undefined' && navigator.onLine === false;
    throw new CoreLoadError(offline ? 'offline' : 'network', offline ? 'You are offline.' : 'The converter could not be downloaded.');
  }
  if (!response.ok) throw new CoreLoadError('network', `The converter could not be downloaded (HTTP ${response.status}).`);

  const header = Number(response.headers.get('Content-Length')) || 0;
  const encoded = (response.headers.get('Content-Encoding') || 'identity') !== 'identity';
  const total = expectedBytes || (encoded ? 0 : header);
  const report = (loaded) => onProgress(loaded, Math.max(total, loaded));
  report(0);

  try {
    if (typeof WebAssembly.compileStreaming === 'function' && response.body && typeof ReadableStream === 'function') {
      const counted = new Response(countingStream(response.body, report), { headers: { 'Content-Type': 'application/wasm' } });
      return await WebAssembly.compileStreaming(counted);
    }
    // No streaming compilation: read with progress, then compile.
    const chunks = [];
    let loaded = 0;
    if (response.body) {
      const reader = response.body.getReader();
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        loaded += value.byteLength;
        report(loaded);
      }
    } else {
      const all = new Uint8Array(await response.arrayBuffer());
      chunks.push(all);
      loaded = all.byteLength;
      report(loaded);
    }
    const bytes = new Uint8Array(loaded);
    let at = 0;
    for (const c of chunks) {
      bytes.set(c, at);
      at += c.byteLength;
    }
    return await WebAssembly.compile(bytes);
  } catch (error) {
    if (error instanceof CoreLoadError) throw error;
    if (error instanceof TypeError && typeof navigator !== 'undefined' && navigator.onLine === false) {
      throw new CoreLoadError('offline', 'The download stopped: you are offline.');
    }
    if (error instanceof TypeError) throw new CoreLoadError('network', 'The download of the converter stopped.');
    throw new CoreLoadError('compile', 'This browser could not start the converter.');
  }
}

// Convertible web app - the dedicated worker that owns one WebAssembly
// instance of the core. The main thread never runs core code.
//
// Messages in  (each carries a numeric `id` that the reply echoes):
//   {type:'init'}
//   {type:'catalogue'}                         -> {formats, version}
//   {type:'detect', blob, name}               -> {format}
//   {type:'targets', source}                   -> {targets}
//   {type:'schema', source, target}            -> {schema}
//   {type:'convert', blob, name, from, to, options}
//        progress while running: {type:'progress', id, fraction, stage}
//        final: {type:'result', id, ok, status, message, buffer, format, extension, mime, warnings}
// Errors come back as {type:'error', id, status, message}.
//
// Cancellation: the core can only be interrupted from its progress
// callback, and a busy worker cannot receive messages (no SharedArrayBuffer
// without cross-origin isolation). The pool therefore hard-cancels a running
// conversion by terminating this worker and starting a fresh one; queued
// files are cancelled before they reach a worker.
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import createConvertibleModule from '../../wasm/convertible.mjs';
import { ConvertibleCore, CoreError, Status } from './binding.js';
import { MAX_INPUT_BYTES, DETECT_BYTES, tooLargeMessage } from '../model/limits.js';

let corePromise = null;

function core() {
  if (!corePromise) corePromise = ConvertibleCore.create(createConvertibleModule);
  return corePromise;
}

/** Streams a Blob into a freshly allocated heap buffer (no second full copy in JS). */
async function readIntoHeap(c, blob) {
  const buffer = c.allocate(blob.size);
  try {
    if (blob.size === 0) return buffer;
    if (typeof blob.stream === 'function') {
      const reader = blob.stream().getReader();
      let offset = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        if (offset + value.length > buffer.size) throw new CoreError(Status.IO, 'the file changed while it was being read');
        c.heapU8.set(value, buffer.ptr + offset);
        offset += value.length;
      }
      if (offset !== buffer.size) throw new CoreError(Status.IO, 'the file changed while it was being read');
    } else {
      buffer.view().set(new Uint8Array(await blob.arrayBuffer()));
    }
    return buffer;
  } catch (error) {
    buffer.free();
    throw error;
  }
}

async function handle(msg) {
  const c = await core();
  switch (msg.type) {
    case 'init':
      return { version: c.version() };
    case 'catalogue':
      return { version: c.version(), formats: c.formats() };
    case 'detect': {
      const head = new Uint8Array(await msg.blob.slice(0, DETECT_BYTES).arrayBuffer());
      return { format: c.detect(head, msg.name ?? null) };
    }
    case 'targets':
      return { targets: c.targets(msg.source) };
    case 'schema':
      return { schema: c.optionsSchema(msg.source, msg.target) };
    case 'convert': {
      if (msg.blob.size > MAX_INPUT_BYTES) throw new CoreError(Status.LIMIT, tooLargeMessage(msg.blob.size));
      let input;
      try {
        input = await readIntoHeap(c, msg.blob);
      } catch (error) {
        if (error instanceof CoreError && error.status === Status.LIMIT) {
          throw new CoreError(Status.LIMIT, tooLargeMessage(msg.blob.size));
        }
        throw error;
      }
      let lastPost = 0;
      try {
        const result = c.convert(input, {
          to: msg.to,
          from: msg.from || '',
          filename: msg.name ?? null,
          options: msg.options ?? null,
          onProgress: (fraction, stage) => {
            const now = Date.now();
            if (now - lastPost > 40 || fraction >= 1) {
              lastPost = now;
              self.postMessage({ type: 'progress', id: msg.id, fraction, stage });
            }
            return false;
          },
        });
        const buffer = result.data ? result.data.buffer : null;
        return {
          transfer: buffer ? [buffer] : [],
          reply: { ...result, data: undefined, buffer },
        };
      } finally {
        input.free();
      }
    }
    default:
      throw new CoreError(Status.INVALID_ARGUMENT, `unknown request ${msg.type}`);
  }
}

self.addEventListener('message', async (event) => {
  const msg = event.data;
  try {
    const out = await handle(msg);
    if (out && out.reply) {
      self.postMessage({ type: msg.type === 'convert' ? 'result' : 'reply', id: msg.id, ...out.reply }, out.transfer);
    } else {
      self.postMessage({ type: 'reply', id: msg.id, ...out });
    }
  } catch (error) {
    const status = error instanceof CoreError ? error.status : Status.INTERNAL;
    const message = error instanceof CoreError ? error.message : String(error?.message || error);
    self.postMessage({ type: 'error', id: msg.id, status, message });
  }
});

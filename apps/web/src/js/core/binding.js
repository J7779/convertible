// Convertible web app - thin typed binding over the core's C ABI (convertible.h)
// compiled to WebAssembly. Works in browsers (inside a Worker) and in Node.
//
// Responsibilities: string marshalling, freeing every returned string with
// cvt_string_free and every result with cvt_result_free, copying result bytes
// out of the WebAssembly heap, and mapping statuses to errors.
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

/** Status codes, identical to enum cvt_status. */
export const Status = Object.freeze({
  OK: 0,
  INVALID_ARGUMENT: 1,
  UNKNOWN_FORMAT: 2,
  UNSUPPORTED_CONVERSION: 3,
  DECODE: 4,
  ENCODE: 5,
  IO: 6,
  CANCELLED: 7,
  LIMIT: 8,
  INTERNAL: 9,
});

const STATUS_NAMES = Object.freeze(Object.fromEntries(Object.entries(Status).map(([k, v]) => [v, k])));

/** Symbolic name of a status code ("DECODE"), or "UNKNOWN". */
export function statusName(status) {
  return STATUS_NAMES[status] ?? 'UNKNOWN';
}

/** An error carrying a core status code. */
export class CoreError extends Error {
  constructor(status, message) {
    super(message || statusName(status));
    this.name = 'CoreError';
    this.status = status;
  }
}

/**
 * @typedef {object} ConvertResult
 * @property {boolean} ok
 * @property {number} status
 * @property {string} message
 * @property {Uint8Array|null} data       a copy owned by JavaScript
 * @property {string} format
 * @property {string} extension
 * @property {string} mime
 * @property {string[]} warnings
 */

/** Wraps one instantiated module. Not shared between threads. */
export class ConvertibleCore {
  /**
   * Instantiates the module.
   * @param {(options?: object) => Promise<object>} factory  default export of convertible.mjs
   * @param {object} [moduleOptions] e.g. {wasmBinary} for Node, {locateFile} for custom layouts
   */
  static async create(factory, moduleOptions = {}) {
    const module = await factory({ print: () => {}, printErr: () => {}, ...moduleOptions });
    return new ConvertibleCore(module);
  }

  constructor(module) {
    this.m = module;
    this._active = null; // {onProgress} of the conversion running right now
    // One persistent C function pointer for progress. Signature: int(double, const char*, void*).
    this._progressFn = module.addFunction((fraction, stagePtr, _user) => {
      const active = this._active;
      if (!active || !active.onProgress) return 0;
      let stop = false;
      try {
        stop = active.onProgress(fraction, module.UTF8ToString(stagePtr)) === true;
      } catch {
        stop = false;
      }
      return stop ? 1 : 0;
    }, 'idii');
  }

  get heapU8() {
    return this.m.HEAPU8;
  }

  // -- marshalling ----------------------------------------------------------

  _withStrings(strings, fn) {
    const ptrs = [];
    try {
      for (const s of strings) {
        if (s === null || s === undefined) {
          ptrs.push(0);
          continue;
        }
        const text = String(s);
        const size = this.m.lengthBytesUTF8(text) + 1;
        const ptr = this.m._malloc(size);
        if (!ptr) throw new CoreError(Status.LIMIT, 'out of memory');
        this.m.stringToUTF8(text, ptr, size);
        ptrs.push(ptr);
      }
      return fn(...ptrs);
    } finally {
      for (const p of ptrs) if (p) this.m._free(p);
    }
  }

  /** Reads and frees a `char*` the library handed over. */
  _takeString(ptr) {
    if (!ptr) return null;
    try {
      return this.m.UTF8ToString(ptr);
    } finally {
      this.m._cvt_string_free(ptr);
    }
  }

  _takeJson(ptr, fallback) {
    const text = this._takeString(ptr);
    if (text === null) return fallback;
    try {
      return JSON.parse(text);
    } catch {
      return fallback;
    }
  }

  // -- library ----------------------------------------------------------------

  version() {
    return this.m.UTF8ToString(this.m._cvt_version());
  }

  /** @returns {Array<object>} every known format, most popular first */
  formats() {
    return this._takeJson(this.m._cvt_formats_json(), []);
  }

  /** @returns {Array<object>} conversion targets for a format id or extension */
  targets(sourceFormatId) {
    return this._withStrings([sourceFormatId], (p) => this._takeJson(this.m._cvt_targets_json(p), []));
  }

  /** @returns {Array<object>} option schema for a conversion */
  optionsSchema(sourceFormatId, targetFormatId) {
    return this._withStrings([sourceFormatId, targetFormatId], (s, t) =>
      this._takeJson(this.m._cvt_options_schema_json(s, t), []),
    );
  }

  /**
   * Detects a format from the beginning of a file (64 KiB is plenty).
   * @param {Uint8Array} bytes
   * @param {string|null} filename
   * @returns {string|null}
   */
  detect(bytes, filename = null) {
    const buffer = this.allocate(bytes.length);
    try {
      buffer.view().set(bytes);
      return this._withStrings([filename], (name) => this._takeString(this.m._cvt_detect(buffer.ptr, bytes.length, name)));
    } finally {
      buffer.free();
    }
  }

  // -- heap buffers -----------------------------------------------------------

  /**
   * Allocates `size` bytes in the module heap. Throws CoreError(LIMIT) when the
   * heap cannot grow that far. `view()` must be re-read after any allocation
   * because the heap may move when it grows.
   */
  allocate(size) {
    if (!Number.isSafeInteger(size) || size < 0) throw new CoreError(Status.INVALID_ARGUMENT, 'invalid size');
    const ptr = size === 0 ? 0 : this.m._malloc(size);
    if (size !== 0 && !ptr) throw new CoreError(Status.LIMIT, 'not enough memory for this file');
    let freed = false;
    return {
      ptr,
      size,
      view: () => (ptr ? this.m.HEAPU8.subarray(ptr, ptr + size) : new Uint8Array(0)),
      free: () => {
        if (!freed && ptr) this.m._free(ptr);
        freed = true;
      },
    };
  }

  // -- conversion ------------------------------------------------------------

  /**
   * Converts bytes. `input` is either a Uint8Array (copied into the heap) or a
   * buffer from allocate() that the caller already filled (not freed here).
   *
   * @param {Uint8Array|{ptr:number,size:number}} input
   * @param {object} opts
   * @param {string} opts.to                 target format id or extension (required)
   * @param {string} [opts.from]             source format; "" or omitted = detect
   * @param {string} [opts.filename]
   * @param {object|string} [opts.options]   option values (object or JSON text)
   * @param {(fraction:number, stage:string) => boolean|void} [opts.onProgress]  return true to cancel
   * @returns {ConvertResult}
   */
  convert(input, { to, from = '', filename = null, options = null, onProgress = null } = {}) {
    if (this._active) throw new CoreError(Status.INTERNAL, 'a conversion is already running on this instance');
    let owned = null;
    let ptr;
    let size;
    if (input instanceof Uint8Array) {
      owned = this.allocate(input.length);
      owned.view().set(input);
      ptr = owned.ptr;
      size = input.length;
    } else {
      ptr = input.ptr;
      size = input.size;
    }
    const optionsJson = options == null ? null : typeof options === 'string' ? options : JSON.stringify(options);
    this._active = { onProgress };
    let result = 0;
    try {
      result = this._withStrings([filename, from, to ?? null, optionsJson], (fn, src, dst, opt) =>
        this.m._cvt_convert(ptr, size, fn, src, dst, opt, this._progressFn, 0),
      );
      return this._readResult(result);
    } finally {
      this._active = null;
      if (result) this.m._cvt_result_free(result);
      if (owned) owned.free();
    }
  }

  _readResult(result) {
    const m = this.m;
    const status = m._cvt_result_status(result);
    const out = {
      ok: status === Status.OK,
      status,
      message: m.UTF8ToString(m._cvt_result_message(result)),
      data: null,
      format: m.UTF8ToString(m._cvt_result_format(result)),
      extension: m.UTF8ToString(m._cvt_result_extension(result)),
      mime: m.UTF8ToString(m._cvt_result_mime(result)),
      warnings: [],
    };
    try {
      out.warnings = JSON.parse(m.UTF8ToString(m._cvt_result_warnings_json(result)));
    } catch {
      out.warnings = [];
    }
    if (out.ok) {
      const dataPtr = m._cvt_result_data(result);
      const dataSize = m._cvt_result_size(result) >>> 0;
      try {
        out.data = new Uint8Array(dataSize);
      } catch {
        return { ...out, ok: false, status: Status.LIMIT, message: 'the result is too large for browser memory' };
      }
      if (dataPtr && dataSize) out.data.set(m.HEAPU8.subarray(dataPtr, dataPtr + dataSize));
    }
    return out;
  }

  /** Bytes currently reserved by the WebAssembly memory (grows, never shrinks). */
  memoryBytes() {
    return this.m.HEAPU8.buffer.byteLength;
  }
}

// End-to-end tests - input files generated on the fly, and independent
// verifiers for downloaded outputs (ZIP central directory, JPEG/PNG magic).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { createHash } from 'node:crypto';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { deflateSync, inflateRawSync } from 'node:zlib';

/** A temporary directory for one test's input files. */
export async function tempDir(prefix = 'cvt-e2e-files-') {
  const dir = await mkdtemp(path.join(os.tmpdir(), prefix));
  return {
    dir,
    /** Writes `data` (string or bytes) to `name` and returns the absolute path. */
    async file(name, data) {
      const file = path.join(dir, name);
      await writeFile(file, data);
      return file;
    },
    remove: () => rm(dir, { recursive: true, force: true, maxRetries: 5 }),
  };
}

export const TEXT = 'Convertible end-to-end test.\nGrüße, 你好, emoji \u{1F600}.\nLine three.\n';

/** Every byte value 0..255, in order. */
export function allBytes() {
  return Buffer.from(Uint8Array.from({ length: 256 }, (_, i) => i));
}

/** Deterministic pseudo-random bytes (xorshift32), so failures reproduce. */
export function pseudoRandom(size, seed = 0x9e3779b9) {
  const out = Buffer.alloc(size);
  let x = seed >>> 0 || 1;
  for (let i = 0; i < size; i += 1) {
    x ^= x << 13;
    x >>>= 0;
    x ^= x >>> 17;
    x ^= x << 5;
    x >>>= 0;
    out[i] = x & 0xff;
  }
  return out;
}

export const sha256 = (data) => createHash('sha256').update(data).digest('hex');
export const md5 = (data) => createHash('md5').update(data).digest('hex');
export const sha1 = (data) => createHash('sha1').update(data).digest('hex');

// ---------------------------------------------------------------------------
// PNG (a valid image built here, for catalogue-conditional image tests)
// ---------------------------------------------------------------------------

const CRC_TABLE = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});

export function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'latin1'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}

/** An RGB PNG with a smooth gradient plus noise (compresses like a photo). */
export function makePng(width = 256, height = 192) {
  const raw = Buffer.alloc((width * 3 + 1) * height);
  const noise = pseudoRandom(width * height * 3, 1234);
  let o = 0;
  for (let y = 0; y < height; y += 1) {
    raw[o++] = 0;
    for (let x = 0; x < width; x += 1) {
      const n = noise[(y * width + x) * 3] & 0x1f;
      raw[o++] = (x * 255) / width + n;
      raw[o++] = (y * 255) / height + n;
      raw[o++] = ((x + y) * 127) / (width + height) + n;
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

export function isJpeg(bytes) {
  return bytes.length > 4 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff && bytes.at(-2) === 0xff && bytes.at(-1) === 0xd9;
}

// ---------------------------------------------------------------------------
// ZIP: parse the central directory and extract entries (stored/deflate)
// ---------------------------------------------------------------------------

/**
 * Parses a ZIP file independently of the app: finds the end-of-central-
 * directory record, walks the central directory, checks each local header
 * and returns [{name, data}] with CRC-32 verified.
 */
export function readZip(buf) {
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 22 - 0xffff); i -= 1) {
    if (buf.readUInt32LE(i) === 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error('no end of central directory record');
  const count = buf.readUInt16LE(eocd + 10);
  const cdSize = buf.readUInt32LE(eocd + 12);
  const cdOffset = buf.readUInt32LE(eocd + 16);
  if (cdOffset + cdSize > eocd) throw new Error('central directory out of range');
  const entries = [];
  let p = cdOffset;
  for (let n = 0; n < count; n += 1) {
    if (buf.readUInt32LE(p) !== 0x02014b50) throw new Error(`bad central directory header at ${p}`);
    const method = buf.readUInt16LE(p + 10);
    const crc = buf.readUInt32LE(p + 16);
    const compressed = buf.readUInt32LE(p + 20);
    const size = buf.readUInt32LE(p + 24);
    const nameLen = buf.readUInt16LE(p + 28);
    const extraLen = buf.readUInt16LE(p + 30);
    const commentLen = buf.readUInt16LE(p + 32);
    const local = buf.readUInt32LE(p + 42);
    const name = buf.subarray(p + 46, p + 46 + nameLen).toString('utf8');
    if (buf.readUInt32LE(local) !== 0x04034b50) throw new Error(`bad local header for ${name}`);
    const start = local + 30 + buf.readUInt16LE(local + 26) + buf.readUInt16LE(local + 28);
    const body = buf.subarray(start, start + compressed);
    let data;
    if (method === 0) data = Buffer.from(body);
    else if (method === 8) data = inflateRawSync(body);
    else throw new Error(`unsupported compression method ${method} for ${name}`);
    if (data.length !== size) throw new Error(`size mismatch for ${name}`);
    if (crc32(data) !== crc) throw new Error(`CRC mismatch for ${name}`);
    entries.push({ name, data, method });
    p += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

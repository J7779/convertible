// Shared test helpers: locate the built site and instantiate the real core.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { existsSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export const WEB_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const DIST = path.resolve(process.env.CVT_WEB_DIST || path.join(WEB_DIR, 'dist'));
export const SRC = path.join(WEB_DIR, 'src');

export function requireDist() {
  if (!existsSync(path.join(DIST, 'wasm', 'convertible.wasm'))) {
    throw new Error(`No build found in ${DIST}. Run apps/web/build.sh first.`);
  }
}

/** Imports a module from the app sources (pure logic modules work in Node). */
export function importSrc(rel) {
  return import(pathToFileURL(path.join(SRC, rel)).href);
}

/** A fresh instance of the real WebAssembly core. */
export async function loadCore() {
  requireDist();
  const { default: factory } = await import(pathToFileURL(path.join(DIST, 'wasm', 'convertible.mjs')).href);
  const { ConvertibleCore } = await importSrc('js/core/binding.js');
  const wasmBinary = await readFile(path.join(DIST, 'wasm', 'convertible.wasm'));
  return ConvertibleCore.create(factory, { wasmBinary });
}

export const utf8 = (s) => new TextEncoder().encode(s);
export const text = (bytes) => new TextDecoder().decode(bytes);

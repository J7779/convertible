// Convertible web app - memory limits of the browser build.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { formatBytes } from './format.js';

/**
 * Largest input the web app accepts. A 32-bit WebAssembly heap tops out at
 * 4 GiB and has to hold the input, the decoded content and the output at the
 * same time, so 1 GB is a safe ceiling on desktop browsers.
 */
export const MAX_INPUT_BYTES = 1_000_000_000;

/** Bytes passed to detection (the header's recommendation). */
export const DETECT_BYTES = 64 * 1024;

export function isTooLarge(size, max = MAX_INPUT_BYTES) {
  return size > max;
}

export function tooLargeMessage(size, max = MAX_INPUT_BYTES) {
  return `This file is ${formatBytes(size)}. The browser version can convert files up to ${formatBytes(max)}, because the whole file has to fit in the page's memory. The desktop apps have no such limit.`;
}

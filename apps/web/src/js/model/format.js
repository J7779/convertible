// Convertible web app - formatting helpers and error mapping (pure, no DOM).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

const UNITS = ['bytes', 'KB', 'MB', 'GB', 'TB'];

/**
 * Human file size in decimal units, the way file managers show it:
 * 0 bytes, 1 byte, 999 bytes, 1 KB, 1.5 KB, 12.3 MB, 1.07 GB.
 */
export function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) return '';
  if (bytes === 1) return '1 byte';
  if (bytes < 1000) return `${Math.round(bytes)} bytes`;
  let value = bytes;
  let unit = 0;
  while (value >= 1000 && unit < UNITS.length - 1) {
    value /= 1000;
    unit += 1;
  }
  // Two significant decimals below 10, one below 100, none above.
  let text;
  if (value < 10) text = value.toFixed(2);
  else if (value < 100) text = value.toFixed(1);
  else text = value.toFixed(0);
  if (text.includes('.')) text = text.replace(/\.?0+$/, '');
  // Rounding may have produced "1000" (e.g. 999.96 KB): carry into the next unit.
  if (text === '1000' && unit < UNITS.length - 1) return `1 ${UNITS[unit + 1]}`;
  return `${text} ${UNITS[unit]}`;
}

/**
 * Change from `before` to `after` bytes.
 * @returns {{saved: number, percent: number, label: string, smaller: boolean}}
 *   percent is the saved share (positive = smaller, negative = larger), rounded.
 */
export function sizeChange(before, after) {
  if (!(before > 0) || !(after >= 0)) return { saved: 0, percent: 0, label: '', smaller: false };
  const saved = before - after;
  const exact = (saved / before) * 100;
  let percent = Math.round(exact);
  if (percent === 0 && saved !== 0) percent = saved > 0 ? 1 : -1; // never claim 0% for a real change
  if (saved === 0) return { saved, percent: 0, label: 'Same size', smaller: false };
  if (saved > 0) return { saved, percent, label: `${percent}% smaller`, smaller: true };
  const growth = -percent;
  const label = after >= before * 2 ? `${trimNumber(after / before)}× larger` : `${growth}% larger`;
  return { saved, percent, label, smaller: false };
}

function trimNumber(x) {
  return x >= 10 ? x.toFixed(0) : x.toFixed(1).replace(/\.0$/, '');
}

/** Friendly titles for core status codes (see cvt_status). */
const STATUS_TITLES = {
  1: 'Some settings are not valid',
  2: 'Unrecognised file',
  3: 'This conversion is not available',
  4: 'The file could not be read',
  5: 'The result could not be written',
  6: 'The file could not be opened',
  7: 'Cancelled',
  8: 'This file is too large',
  9: 'Something went wrong',
};

export function statusTitle(status) {
  return STATUS_TITLES[status] ?? 'Something went wrong';
}

/** Capitalises the core's sentence-style message and ends it with a full stop. */
export function friendlyMessage(message) {
  const text = String(message ?? '').trim();
  if (!text) return '';
  const first = text[0].toUpperCase() + text.slice(1);
  return /[.!?…]$/.test(first) ? first : `${first}.`;
}

/** "photo.png" + "jpg" -> "photo.jpg"; keeps hidden-file names sensible. */
export function outputName(inputName, extension) {
  const name = String(inputName || 'converted');
  const dot = name.lastIndexOf('.');
  const stem = dot > 0 ? name.slice(0, dot) : name;
  return extension ? `${stem}.${extension}` : stem;
}

/** Makes `name` unique among `taken` (a Set) as "name (2).ext". Adds it to the set. */
export function uniqueName(name, taken) {
  if (!taken.has(name)) {
    taken.add(name);
    return name;
  }
  const dot = name.lastIndexOf('.');
  const stem = dot > 0 ? name.slice(0, dot) : name;
  const ext = dot > 0 ? name.slice(dot) : '';
  for (let i = 2; ; i += 1) {
    const candidate = `${stem} (${i})${ext}`;
    if (!taken.has(candidate)) {
      taken.add(candidate);
      return candidate;
    }
  }
}

/** Upper-case short label for a format badge: "png" -> "PNG", "data-uri" -> "DATA-URI". */
export function formatLabel(id) {
  return String(id || '?').toUpperCase();
}

export function plural(n, one, many = `${one}s`) {
  return `${n} ${n === 1 ? one : many}`;
}

export function percentText(fraction) {
  const p = Math.max(0, Math.min(1, Number(fraction) || 0));
  return `${Math.floor(p * 100)}%`;
}

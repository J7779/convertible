// Convertible web app - browser file I/O: drops (files and folders), paste,
// saving (File System Access API with a download fallback), share and open.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { uniqueName } from '../model/format.js';

const IGNORED = new Set(['.DS_Store', 'Thumbs.db', 'desktop.ini']);

/** False for system clutter (.DS_Store, Thumbs.db, desktop.ini, AppleDouble files). */
export function isUserFile(file) {
  return file && !IGNORED.has(file.name) && !file.name.startsWith('._');
}

function readAllEntries(reader) {
  return new Promise((resolve) => {
    const all = [];
    const next = () =>
      reader.readEntries(
        (batch) => {
          if (!batch.length) resolve(all);
          else {
            all.push(...batch);
            next();
          }
        },
        () => resolve(all),
      );
    next();
  });
}

async function walkEntry(entry, out, depth = 0) {
  if (!entry || depth > 32) return;
  if (entry.isFile) {
    const file = await new Promise((resolve) => entry.file(resolve, () => resolve(null)));
    if (isUserFile(file)) out.push(file);
  } else if (entry.isDirectory) {
    const children = await readAllEntries(entry.createReader());
    for (const child of children) await walkEntry(child, out, depth + 1);
  }
}

/** Files from a drop, expanding dropped folders recursively. */
export async function filesFromDataTransfer(dt) {
  const items = dt.items ? [...dt.items].filter((i) => i.kind === 'file') : [];
  const entries = items.map((i) => (typeof i.webkitGetAsEntry === 'function' ? i.webkitGetAsEntry() : null));
  if (entries.length && entries.every(Boolean)) {
    const out = [];
    for (const entry of entries) await walkEntry(entry, out);
    return out;
  }
  return [...(dt.files || [])].filter(isUserFile);
}

/** Files (or text, as a .txt file) from a paste event. */
export function filesFromClipboard(event) {
  const cd = event.clipboardData;
  if (!cd) return [];
  const files = [...(cd.files || [])].filter(isUserFile);
  if (files.length) return files;
  const text = cd.getData('text/plain');
  if (text) return [new File([text], 'Pasted text.txt', { type: 'text/plain' })];
  return [];
}

export function hasFiles(event) {
  const types = event.dataTransfer?.types;
  return Boolean(types && [...types].includes('Files'));
}

function download(blob, name) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.rel = 'noopener';
  a.hidden = true;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

function pickerTypes(name, mime) {
  const dot = name.lastIndexOf('.');
  if (dot <= 0) return undefined;
  const ext = name.slice(dot);
  const type = (mime || 'application/octet-stream').split(';')[0];
  if (!/^[a-z]+\/[a-z0-9.+-]+$/i.test(type)) return undefined;
  return [{ description: ext.slice(1).toUpperCase(), accept: { [type]: [ext] } }];
}

/**
 * Saves one file. Uses the native save dialog where the browser has one,
 * otherwise a download. Resolves to "saved", "downloaded" or "cancelled".
 */
export async function saveFile(blob, name, mime) {
  if (typeof window.showSaveFilePicker === 'function') {
    try {
      let handle;
      try {
        handle = await window.showSaveFilePicker({ suggestedName: name, types: pickerTypes(name, mime) });
      } catch (error) {
        if (error && error.name === 'TypeError') handle = await window.showSaveFilePicker({ suggestedName: name });
        else throw error;
      }
      const writable = await handle.createWritable();
      await writable.write(blob);
      await writable.close();
      return 'saved';
    } catch (error) {
      if (error && error.name === 'AbortError') return 'cancelled';
      // SecurityError (e.g. no user activation left) and friends: fall back.
    }
  }
  download(blob, name);
  return 'downloaded';
}

/**
 * Saves several files: into a folder the user picks where the browser
 * supports it, otherwise as individual downloads.
 * @param {{blob: Blob, name: string}[]} files
 */
export async function saveAll(files) {
  if (typeof window.showDirectoryPicker === 'function') {
    let dir = null;
    try {
      dir = await window.showDirectoryPicker({ mode: 'readwrite', id: 'convertible-save' });
    } catch (error) {
      if (error && error.name === 'AbortError') return 'cancelled';
    }
    if (dir) {
      const taken = new Set();
      for await (const [n] of dir.entries()) taken.add(n);
      for (const f of files) {
        const handle = await dir.getFileHandle(uniqueName(f.name, taken), { create: true });
        const w = await handle.createWritable();
        await w.write(f.blob);
        await w.close();
      }
      return 'saved';
    }
  }
  const taken = new Set();
  for (const f of files) {
    download(f.blob, uniqueName(f.name, taken));
    // Browsers throttle bursts of downloads; a short gap keeps every file.
    await new Promise((r) => setTimeout(r, 350));
  }
  return 'downloaded';
}

export function canShareFile(blob, name, mime) {
  if (typeof navigator.canShare !== 'function' || typeof File !== 'function') return false;
  try {
    return navigator.canShare({ files: [new File([blob], name, { type: mime || blob.type })] });
  } catch {
    return false;
  }
}

export async function shareFile(blob, name, mime) {
  try {
    await navigator.share({ files: [new File([blob], name, { type: mime || blob.type })], title: name });
    return 'shared';
  } catch (error) {
    return error && error.name === 'AbortError' ? 'cancelled' : 'failed';
  }
}

const VIEWABLE = /^(text\/plain|text\/csv|application\/json|image\/(png|jpeg|gif|webp|avif|svg\+xml|bmp)|application\/pdf|audio\/|video\/)/;

export function canOpen(mime) {
  return VIEWABLE.test(mime || '');
}

/** Opens a result in a new browser tab (for formats the browser can show). */
export function openInTab(blob, mime) {
  const type = /^text\/|json/.test(mime) ? 'text/plain;charset=utf-8' : mime;
  const url = URL.createObjectURL(new Blob([blob], { type }));
  window.open(url, '_blank', 'noopener');
  setTimeout(() => URL.revokeObjectURL(url), 120_000);
}

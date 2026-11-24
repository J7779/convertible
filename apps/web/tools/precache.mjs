// Convertible web app - service-worker precache manifest generation.
// Pure helpers shared by tools/assemble.mjs and the tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { createHash } from 'node:crypto';
import { readdir, readFile } from 'node:fs/promises';
import path from 'node:path';

/** Files that are never precached: the worker itself and host configuration. */
export const PRECACHE_EXCLUDE = new Set(['sw.js', '_headers', '.DS_Store']);

/** Recursively lists the files of `dir` as sorted POSIX paths relative to it. */
export async function listFiles(dir) {
  const out = [];
  async function walk(rel) {
    const entries = await readdir(path.join(dir, rel), { withFileTypes: true });
    for (const entry of entries) {
      const child = rel ? `${rel}/${entry.name}` : entry.name;
      if (entry.isDirectory()) await walk(child);
      else if (entry.isFile()) out.push(child);
    }
  }
  await walk('');
  return out.sort();
}

/**
 * Builds the manifest for the files of a dist directory:
 * {version, files: [{url, hash, size}]} with URLs relative to the service
 * worker scope ("./index.html"). The version is a digest of every path and
 * content hash, so any change to any file yields a new cache.
 */
export async function buildManifest(distDir) {
  const files = (await listFiles(distDir)).filter((f) => !PRECACHE_EXCLUDE.has(path.posix.basename(f)));
  const entries = [];
  const all = createHash('sha256');
  for (const file of files) {
    const data = await readFile(path.join(distDir, file));
    const hash = createHash('sha256').update(data).digest('hex').slice(0, 16);
    entries.push({ url: `./${file}`, hash, size: data.length });
    all.update(`${file}\0${hash}\n`);
  }
  return { version: all.digest('hex').slice(0, 12), files: entries };
}

const START = '/* precache:start */';
const END = '/* precache:end */';

/** Substitutes the manifest into the service-worker template. */
export function injectManifest(template, manifest) {
  const a = template.indexOf(START);
  const b = template.indexOf(END);
  if (a < 0 || b < a) throw new Error('service worker template lacks the precache markers');
  const body = `${START}\nconst CACHE_VERSION = ${JSON.stringify(manifest.version)};\nconst PRECACHE = ${JSON.stringify(
    manifest.files.map((f) => f.url),
    null,
    1,
  )};\n${END}`;
  return template.slice(0, a) + body + template.slice(b + END.length);
}

/** Reads {version, urls} back out of a generated sw.js. */
export function readManifestFromSw(text) {
  const version = /const CACHE_VERSION = "([^"]+)";/.exec(text)?.[1] ?? null;
  const list = /const PRECACHE = (\[[\s\S]*?\]);/.exec(text)?.[1];
  return { version, urls: list ? JSON.parse(list) : [] };
}

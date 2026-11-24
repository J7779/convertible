// Size formatting, naming, error mapping, limits and settings tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { importSrc } from './helpers.mjs';

const { formatBytes, sizeChange, statusTitle, friendlyMessage, outputName, uniqueName, formatLabel, plural, percentText } = await importSrc('js/model/format.js');
const { isTooLarge, tooLargeMessage, MAX_INPUT_BYTES } = await importSrc('js/model/limits.js');
const { loadSettings, saveSettings, sanitizeSettings, DEFAULT_SETTINGS } = await importSrc('js/model/settings.js');
const { Status } = await importSrc('js/core/binding.js');

test('formatBytes', () => {
  assert.equal(formatBytes(0), '0 bytes');
  assert.equal(formatBytes(1), '1 byte');
  assert.equal(formatBytes(999), '999 bytes');
  assert.equal(formatBytes(1000), '1 KB');
  assert.equal(formatBytes(1500), '1.5 KB');
  assert.equal(formatBytes(2450), '2.45 KB');
  assert.equal(formatBytes(12_345_678), '12.3 MB');
  assert.equal(formatBytes(999_960), '1 MB');
  assert.equal(formatBytes(1_000_000_000), '1 GB');
  assert.equal(formatBytes(5e12), '5 TB');
  assert.equal(formatBytes(-1), '');
  assert.equal(formatBytes(Number.NaN), '');
});

test('sizeChange', () => {
  assert.deepEqual(sizeChange(1000, 400), { saved: 600, percent: 60, label: '60% smaller', smaller: true });
  assert.equal(sizeChange(1000, 1000).label, 'Same size');
  assert.equal(sizeChange(1000, 1350).label, '35% larger');
  assert.equal(sizeChange(1000, 3000).label, '3× larger');
  assert.equal(sizeChange(1000, 999).label, '1% smaller', 'a real change is never 0%');
  assert.equal(sizeChange(0, 10).label, '');
});

test('status titles cover every core status', () => {
  for (const code of Object.values(Status)) {
    if (code === Status.OK) continue;
    assert.ok(statusTitle(code).length > 3, String(code));
  }
  assert.equal(statusTitle(Status.LIMIT), 'This file is too large');
  assert.equal(statusTitle(123), 'Something went wrong');
  assert.equal(friendlyMessage('invalid Base64 character at offset 0'), 'Invalid Base64 character at offset 0.');
  assert.equal(friendlyMessage('Done!'), 'Done!');
  assert.equal(friendlyMessage(''), '');
});

test('output names', () => {
  assert.equal(outputName('photo.png', 'jpg'), 'photo.jpg');
  assert.equal(outputName('archive.tar.gz', 'zip'), 'archive.tar.zip');
  assert.equal(outputName('.bashrc', 'b64'), '.bashrc.b64');
  assert.equal(outputName('README', 'hex'), 'README.hex');
  const taken = new Set();
  assert.equal(uniqueName('a.txt', taken), 'a.txt');
  assert.equal(uniqueName('a.txt', taken), 'a (2).txt');
  assert.equal(uniqueName('a.txt', taken), 'a (3).txt');
  assert.equal(uniqueName('noext', taken), 'noext');
  assert.equal(uniqueName('noext', taken), 'noext (2)');
  assert.equal(formatLabel('data-uri'), 'DATA-URI');
  assert.equal(plural(1, 'file'), '1 file');
  assert.equal(plural(3, 'match', 'matches'), '3 matches');
  assert.equal(percentText(0.426), '42%');
  assert.equal(percentText(2), '100%');
});

test('memory limit message', () => {
  assert.equal(isTooLarge(MAX_INPUT_BYTES), false);
  assert.equal(isTooLarge(MAX_INPUT_BYTES + 1), true);
  assert.match(tooLargeMessage(2.5e9), /2\.5 GB.*1 GB/);
});

test('settings are sanitised and persisted', () => {
  const store = new Map();
  const storage = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, v) };
  assert.deepEqual(loadSettings(storage), DEFAULT_SETTINGS);
  saveSettings(storage, { theme: 'light', preset: 'small', stripMetadata: false, extra: 1 });
  assert.deepEqual(loadSettings(storage), { theme: 'light', preset: 'small', stripMetadata: false });
  assert.deepEqual(sanitizeSettings({ theme: 'neon', preset: 7 }), DEFAULT_SETTINGS);
  store.set('convertible.settings.v1', '{broken');
  assert.deepEqual(loadSettings(storage), DEFAULT_SETTINGS);
  assert.deepEqual(loadSettings(null), DEFAULT_SETTINGS);
  const throwing = { getItem: () => { throw new Error('denied'); }, setItem: () => { throw new Error('denied'); } };
  assert.deepEqual(loadSettings(throwing), DEFAULT_SETTINGS);
  saveSettings(throwing, DEFAULT_SETTINGS);
});

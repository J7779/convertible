// Convertible web app - user preferences. Only these three preferences are
// stored (in localStorage); files and conversion history never are.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

export const DEFAULT_SETTINGS = Object.freeze({ theme: 'system', preset: 'balanced', stripMetadata: true });

const KEY = 'convertible.settings.v1';
const THEMES = new Set(['system', 'dark', 'light']);
const PRESETS = new Set(['high', 'balanced', 'small']);

export function sanitizeSettings(raw) {
  const out = { ...DEFAULT_SETTINGS };
  if (raw && typeof raw === 'object') {
    if (THEMES.has(raw.theme)) out.theme = raw.theme;
    if (PRESETS.has(raw.preset)) out.preset = raw.preset;
    if (typeof raw.stripMetadata === 'boolean') out.stripMetadata = raw.stripMetadata;
  }
  return out;
}

export function loadSettings(storage) {
  try {
    return sanitizeSettings(JSON.parse(storage?.getItem(KEY) ?? 'null'));
  } catch {
    return { ...DEFAULT_SETTINGS };
  }
}

export function saveSettings(storage, settings) {
  try {
    storage?.setItem(KEY, JSON.stringify(sanitizeSettings(settings)));
  } catch {
    /* storage unavailable (private mode): settings last for this session */
  }
}

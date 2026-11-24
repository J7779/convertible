// Convertible web app - option schema (cvt_options_schema_json) -> form model.
// Pure, no DOM: the views render whatever this returns.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

export const PRESETS = Object.freeze([
  { value: 'high', label: 'High' },
  { value: 'balanced', label: 'Balanced' },
  { value: 'small', label: 'Small' },
]);

/** Keys the app sets from its own settings rather than per file. */
export const APP_MANAGED_KEYS = new Set(['strip_metadata']);

const SLIDER_MAX_STEPS = 1000;

function humanizeChoice(value) {
  const text = String(value);
  if (/^[b-df-hj-np-tv-xz]+$/.test(text)) return text.toUpperCase(); // acronyms without vowels: lf, crlf, rgb
  if (/^(utf|latin|windows)/i.test(text)) return text.toUpperCase().replace('LATIN', 'Latin-');
  return text.charAt(0).toUpperCase() + text.slice(1).replace(/[_-]/g, ' ');
}

/**
 * Builds a control description for one schema entry.
 * kinds: slider | stepper | switch | menu | field
 */
export function controlFor(spec) {
  const base = {
    key: spec.key,
    label: spec.label || spec.key,
    description: spec.description || '',
    advanced: spec.category === 'advanced',
    default: spec.default,
  };
  switch (spec.type) {
    case 'int': {
      const min = Number.isFinite(spec.min) ? spec.min : null;
      const max = Number.isFinite(spec.max) ? spec.max : null;
      const bounded = min !== null && max !== null && max > min;
      const steps = bounded ? max - min : Infinity;
      return { ...base, kind: bounded && steps <= SLIDER_MAX_STEPS ? 'slider' : 'stepper', type: 'int', min, max, step: 1 };
    }
    case 'float': {
      const min = Number.isFinite(spec.min) ? spec.min : null;
      const max = Number.isFinite(spec.max) ? spec.max : null;
      const bounded = min !== null && max !== null && max > min;
      const step = bounded ? niceStep((max - min) / 100) : 0.1;
      return { ...base, kind: bounded ? 'slider' : 'stepper', type: 'float', min, max, step };
    }
    case 'bool':
      return { ...base, kind: 'switch', type: 'bool', default: Boolean(spec.default) };
    case 'enum':
      return {
        ...base,
        kind: 'menu',
        type: 'enum',
        choices: (spec.choices || []).map((c) => ({ value: c, label: humanizeChoice(c) })),
      };
    default:
      return { ...base, kind: 'field', type: 'string', default: spec.default ?? '' };
  }
}

function niceStep(raw) {
  if (!(raw > 0)) return 0.1;
  const exp = Math.pow(10, Math.floor(Math.log10(raw)));
  for (const m of [1, 2, 5, 10]) if (raw <= m * exp) return m * exp;
  return 10 * exp;
}

/**
 * Splits a schema into the preset segmented control, basic controls and
 * advanced controls. `preset` is non-null only when the schema offers the
 * standard "preset" enum with high/balanced/small.
 */
export function buildControls(schema) {
  const list = Array.isArray(schema) ? schema : [];
  let preset = null;
  const basic = [];
  const advanced = [];
  const managed = [];
  for (const spec of list) {
    if (!spec || typeof spec.key !== 'string') continue;
    if (spec.key === 'preset' && spec.type === 'enum') {
      const offered = PRESETS.filter((p) => (spec.choices || []).includes(p.value));
      if (offered.length) {
        preset = { key: 'preset', choices: offered, default: spec.default ?? 'balanced', description: spec.description || '' };
        continue;
      }
    }
    const control = controlFor(spec);
    if (APP_MANAGED_KEYS.has(spec.key)) managed.push(control);
    else if (control.advanced) advanced.push(control);
    else basic.push(control);
  }
  return { preset, basic, advanced, managed, empty: !preset && !basic.length && !advanced.length && !managed.length };
}

/** Coerces and clamps a UI value for a control; returns undefined when invalid. */
export function coerce(control, raw) {
  switch (control.type) {
    case 'int': {
      const n = typeof raw === 'number' ? raw : Number.parseInt(String(raw).trim(), 10);
      if (!Number.isFinite(n)) return undefined;
      let v = Math.round(n);
      if (control.min !== null) v = Math.max(control.min, v);
      if (control.max !== null) v = Math.min(control.max, v);
      return v;
    }
    case 'float': {
      const n = typeof raw === 'number' ? raw : Number.parseFloat(String(raw).trim());
      if (!Number.isFinite(n)) return undefined;
      let v = n;
      if (control.min !== null) v = Math.max(control.min, v);
      if (control.max !== null) v = Math.min(control.max, v);
      return v;
    }
    case 'bool':
      return raw === true || raw === 'true' || raw === 'on' || raw === 1;
    case 'enum':
      return control.choices.some((c) => c.value === raw) ? raw : undefined;
    default:
      return raw == null ? undefined : String(raw);
  }
}

/**
 * The options object for cvt_convert: the user's per-file values that differ
 * from the defaults, the preset, and app-level settings (strip_metadata) when
 * the schema declares them. Unknown keys are dropped.
 */
export function optionsFor(model, values = {}, settings = {}) {
  const out = {};
  const all = [...model.basic, ...model.advanced];
  for (const control of all) {
    if (APP_MANAGED_KEYS.has(control.key)) continue;
    if (!(control.key in values)) continue;
    const v = coerce(control, values[control.key]);
    if (v !== undefined && v !== control.default) out[control.key] = v;
  }
  if (model.preset) {
    const preset = values.preset ?? settings.preset;
    if (preset && model.preset.choices.some((c) => c.value === preset)) out.preset = preset;
  }
  for (const control of model.managed) {
    if (control.key === 'strip_metadata' && 'stripMetadata' in settings) out.strip_metadata = Boolean(settings.stripMetadata);
  }
  return out;
}

/** Current value of a control for display: user value, else the default. */
export function valueOf(control, values = {}) {
  return control.key in values ? values[control.key] : control.default;
}

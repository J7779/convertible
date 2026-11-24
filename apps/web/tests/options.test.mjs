// Option schema -> controls model tests (synthetic schemas plus the real catalogue).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { importSrc, loadCore } from './helpers.mjs';

const { buildControls, controlFor, coerce, optionsFor, valueOf } = await importSrc('js/model/options.js');

const schema = [
  { key: 'quality', label: 'Quality', type: 'int', default: 85, min: 1, max: 100, choices: [], description: 'Higher is better.', category: 'basic' },
  { key: 'preset', label: 'Preset', type: 'enum', default: 'balanced', min: null, max: null, choices: ['high', 'balanced', 'small'], description: '', category: 'basic' },
  { key: 'strip_metadata', label: 'Strip metadata', type: 'bool', default: true, min: null, max: null, choices: [], description: '', category: 'basic' },
  { key: 'lossless', label: 'Lossless', type: 'bool', default: false, min: null, max: null, choices: [], description: '', category: 'advanced' },
  { key: 'line_ending', label: 'Line ending', type: 'enum', default: 'lf', min: null, max: null, choices: ['lf', 'crlf'], description: '', category: 'advanced' },
  { key: 'max_width', label: 'Max width', type: 'int', default: 0, min: 0, max: 100000, choices: [], description: '', category: 'advanced' },
  { key: 'scale', label: 'Scale', type: 'float', default: 1, min: 0.01, max: 10, choices: [], description: '', category: 'advanced' },
  { key: 'background', label: 'Background', type: 'string', default: 'ffffff', min: null, max: null, choices: [], description: '', category: 'advanced' },
];

test('controls are chosen by type', () => {
  const m = buildControls(schema);
  assert.deepEqual(m.preset.choices.map((c) => c.value), ['high', 'balanced', 'small']);
  assert.deepEqual(m.basic.map((c) => [c.key, c.kind]), [['quality', 'slider']]);
  assert.deepEqual(
    m.advanced.map((c) => [c.key, c.kind]),
    [['lossless', 'switch'], ['line_ending', 'menu'], ['max_width', 'stepper'], ['scale', 'slider'], ['background', 'field']],
  );
  assert.deepEqual(m.managed.map((c) => c.key), ['strip_metadata']);
  assert.equal(m.empty, false);
  assert.equal(controlFor(schema[4]).choices[1].label, 'CRLF');
  assert.ok(controlFor(schema[6]).step > 0);
});

test('empty and malformed schemas', () => {
  assert.equal(buildControls([]).empty, true);
  assert.equal(buildControls(null).empty, true);
  assert.equal(buildControls([{ nokey: 1 }, null]).empty, true);
  // A "preset" without the standard values is an ordinary menu.
  const odd = buildControls([{ key: 'preset', type: 'enum', choices: ['fast', 'slow'], default: 'fast', category: 'basic' }]);
  assert.equal(odd.preset, null);
  assert.equal(odd.basic[0].kind, 'menu');
});

test('coerce clamps and validates', () => {
  const [q] = buildControls(schema).basic;
  assert.equal(coerce(q, '250'), 100);
  assert.equal(coerce(q, 0), 1);
  assert.equal(coerce(q, '42.6'), 42);
  assert.equal(coerce(q, 'abc'), undefined);
  const m = buildControls(schema);
  const menu = m.advanced.find((c) => c.key === 'line_ending');
  assert.equal(coerce(menu, 'crlf'), 'crlf');
  assert.equal(coerce(menu, 'cr'), undefined);
  const sw = m.advanced.find((c) => c.key === 'lossless');
  assert.equal(coerce(sw, 'true'), true);
  assert.equal(coerce(sw, false), false);
});

test('optionsFor sends only changed values, the preset and app settings', () => {
  const m = buildControls(schema);
  assert.deepEqual(optionsFor(m, {}, { preset: 'balanced', stripMetadata: true }), { preset: 'balanced', strip_metadata: true });
  assert.deepEqual(
    optionsFor(m, { quality: 85, lossless: true, unknown: 1, strip_metadata: false, preset: 'small' }, { preset: 'high', stripMetadata: false }),
    { lossless: true, preset: 'small', strip_metadata: false },
  );
  assert.deepEqual(optionsFor(buildControls([]), { quality: 3 }, { preset: 'small', stripMetadata: true }), {});
  assert.equal(valueOf(m.basic[0], {}), 85);
  assert.equal(valueOf(m.basic[0], { quality: 3 }), 3);
});

test('every schema in the real catalogue builds a model whose defaults convert', async () => {
  const core = await loadCore();
  const enc = new TextEncoder();
  let checked = 0;
  for (const target of core.targets('txt')) {
    const model = buildControls(core.optionsSchema('txt', target.id));
    for (const c of [...model.basic, ...model.advanced, ...model.managed]) {
      assert.ok(['slider', 'stepper', 'switch', 'menu', 'field'].includes(c.kind));
      if (c.kind === 'menu') assert.ok(c.choices.length > 0);
    }
    const options = optionsFor(model, {}, { preset: 'small', stripMetadata: true });
    // 16 bytes: fits encodings with block sizes (Z85 needs a multiple of 4).
    const r = core.convert(enc.encode('Options test 12\n'), { from: 'txt', to: target.id, options });
    // Content-dependent refusals are fine (DECODE: the text is not, say, a
    // valid host name for IDNA; ENCODE: it cannot be represented), but the
    // options themselves must be accepted: never INVALID_ARGUMENT.
    assert.ok([0, 4, 5].includes(r.status), `${target.id}: ${r.status} ${r.message}`);
    checked += 1;
  }
  assert.ok(checked > 3);
});

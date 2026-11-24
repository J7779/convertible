// Real conversions through the real core in the browser, verified in Node:
// progress, before/after sizes, download names and bytes, options rendered
// from the core's schema changing the output, re-encoding, large files and
// the native save dialog path.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { card, chooseFiles, chooseTarget, convert, openAdvanced, openApp, optionLabels, outputName, save, setOption, tid, waitForCard, waitForReady } from './lib/app.mjs';
import { TEXT, allBytes, md5, pseudoRandom, sha1, sha256, tempDir } from './lib/fixtures.mjs';
import { core, e2e, needsFormats, server, suite } from './lib/harness.mjs';

const { formatBytes, sizeChange } = await import('../src/js/model/format.js');
const { APP_MANAGED_KEYS } = await import('../src/js/model/options.js');

/** The extension the core gives this conversion's output (from a real conversion in Node). */
const ext = (from, to, data = Buffer.from('a')) => {
  const r = core.convert(new Uint8Array(data), { from, to });
  assert.equal(r.status, 0, `${from} -> ${to} works in Node: ${r.message}`);
  return r.extension;
};
const detect = (data, name) => core.detect(new Uint8Array(data.subarray(0, 65536)), name) ?? 'bin';

/** Starts recording a card's state, progress bar value and stage text on every DOM change. */
function traceCard(page, name) {
  return page.evaluate((n) => {
    const li = [...document.querySelectorAll('[data-testid="file-card"]')].find((el) => el.querySelector('.file-name')?.textContent === n);
    window.__trace = [];
    const record = () => {
      const bar = li.querySelector('[role="progressbar"]');
      const last = window.__trace.at(-1);
      const entry = { state: li.dataset.state, value: bar ? Number(bar.getAttribute('aria-valuenow')) : null, stage: li.querySelector('[data-role="stage"]')?.textContent ?? null };
      if (!last || last.state !== entry.state || last.value !== entry.value || last.stage !== entry.stage) window.__trace.push(entry);
    };
    record();
    new MutationObserver(record).observe(li, { attributes: true, childList: true, subtree: true, characterData: true });
  }, name);
}

/** The id of the input a card labels `label` (option controls are rendered from the schema). */
function controlId(page, name, label) {
  return page.withElement({ card: name, sel: '.card-options' }, (root, l) => {
    const el = [...root.querySelectorAll('label.field-label')].find((x) => x.textContent === l);
    if (!el) throw new Error(`no control labelled ${l}`);
    return el.htmlFor;
  }, label);
}

/** A control's current state, looked up by label (cards re-render, so ids change). */
function control(page, name, label) {
  return page.evaluate((n, l) => {
    const li = [...document.querySelectorAll('[data-testid="file-card"]')].find((el) => el.querySelector('.file-name')?.textContent === n);
    const lab = li && [...li.querySelectorAll('label.field-label')].find((x) => x.textContent === l);
    const el = lab && document.getElementById(lab.htmlFor);
    return el ? { id: el.id, value: el.value, checked: el.checked, focused: document.activeElement === el, shown: document.querySelector(`output[for="${el.id}"]`)?.textContent ?? null } : null;
  }, name, label);
}

/** Waits until a control (by label) has `want.value` and/or `want.checked`. */
async function waitForControl(page, name, label, want) {
  const deadline = Date.now() + 10_000;
  for (;;) {
    const c = await control(page, name, label);
    if (c && (want.value === undefined || c.value === want.value) && (want.checked === undefined || c.checked === want.checked)) return c;
    if (Date.now() > deadline) assert.fail(`control ${label} of ${name} never reached ${JSON.stringify(want)} (now ${JSON.stringify(c)})`);
    await new Promise((r) => setTimeout(r, 25));
  }
}

async function addOne(ctx, name, data, opts = {}) {
  const files = await tempDir();
  const path = await files.file(name, data);
  const page = await openApp(ctx, server(), opts);
  await chooseFiles(page, [path]);
  await waitForReady(page, [name]);
  return { files, page, path };
}

const has = (source, target) => core.targets(source).some((t) => t.id === target);

suite('conversions', () => {
  e2e('text to Base64: progress, before/after sizes, download name and bytes', async ({ ctx }) => {
    const input = Buffer.concat([Buffer.from(TEXT), pseudoRandom(3_000_000)]);
    const { files, page } = await addOne(ctx, 'mixed.dat', input);
    const source = detect(input, 'mixed.dat');
    await chooseTarget(page, 'mixed.dat', 'base64');
    await traceCard(page, 'mixed.dat');
    await convert(page, 'mixed.dat');

    const trace = await page.evaluate(() => window.__trace);
    const states = trace.map((t) => t.state).filter((s, i, a) => s !== a[i - 1]);
    assert.deepEqual(states.slice(-3), ['queued', 'converting', 'done'], `state sequence ${states.join(' > ')}`);
    const values = trace.filter((t) => t.state === 'converting' && t.value !== null).map((t) => t.value);
    assert.ok(values.length >= 1, 'a progress bar is shown while converting');
    for (let i = 1; i < values.length; i += 1) assert.ok(values[i] >= values[i - 1], `progress never goes back: ${values.join(',')}`);
    assert.ok(trace.some((t) => t.state === 'converting' && t.stage && t.stage.endsWith('…')), 'a stage is named');

    const expected = core.convert(new Uint8Array(input), { from: source, to: 'base64' });
    const outName = outputName('mixed.dat', expected.extension);
    assert.equal(await page.text({ card: 'mixed.dat', sel: '.result-name' }), outName);
    const change = sizeChange(input.length, expected.data.length);
    assert.equal(await page.text({ card: 'mixed.dat', sel: tid('result-sizes') }), `${formatBytes(input.length)} ${formatBytes(expected.data.length)} ${change.label}`.trim());

    const d = await save(page, ctx, 'mixed.dat', outName);
    assert.equal(d.bytes.length, expected.data.length);
    assert.ok(Buffer.from(d.bytes.toString('latin1'), 'base64').equals(input), 'Base64 decodes to the input');
    await page.waitForText(`Downloading ${outName}`, { selector: '#toasts' });
    await files.remove();
  });

  e2e('every byte value survives hex and back through the app', async ({ ctx }) => {
    const input = allBytes();
    const { files, page } = await addOne(ctx, 'bytes.bin', input);
    await chooseTarget(page, 'bytes.bin', 'hex');
    await convert(page, 'bytes.bin');
    const hexName = outputName('bytes.bin', ext('bin', 'hex'));
    const hex = await save(page, ctx, 'bytes.bin', hexName);
    assert.ok(Buffer.from(hex.bytes.toString('latin1').replace(/\s+/g, ''), 'hex').equals(input));

    // Feed the downloaded hex back in: detected as hex, converted to raw bytes.
    const again = await files.file(hexName, hex.bytes);
    await chooseFiles(page, [again]);
    await waitForReady(page, [hexName]);
    assert.equal((await card(page, hexName)).badge, detect(hex.bytes, hexName).toUpperCase());
    await chooseTarget(page, hexName, 'bin');
    await convert(page, hexName);
    const binName = outputName(hexName, ext(detect(hex.bytes, hexName), 'bin', hex.bytes));
    const back = await save(page, ctx, hexName, binName);
    assert.ok(back.bytes.equals(input), 'all 256 byte values come back unchanged');
    await files.remove();
  });

  e2e('checksums of a multi-megabyte file equal node:crypto', async ({ ctx }) => {
    const input = pseudoRandom(6 * 1024 * 1024 + 7, 42);
    const { files, page } = await addOne(ctx, 'big.bin', input);
    await chooseTarget(page, 'big.bin', 'checksums');
    await convert(page, 'big.bin');
    const d = await save(page, ctx, 'big.bin', outputName('big.bin', ext('bin', 'checksums')));
    const report = d.bytes.toString('utf8');
    assert.ok(report.includes(sha256(input)), 'SHA-256');
    assert.ok(report.includes(sha1(input)), 'SHA-1');
    assert.ok(report.includes(md5(input)), 'MD5');
    assert.ok(report.includes(String(input.length)), 'size');
    await files.remove();
  });

  e2e('a 24 MB file converts to Base64 and back to identical bytes', async ({ ctx }) => {
    const input = pseudoRandom(24 * 1024 * 1024, 7);
    const { files, page } = await addOne(ctx, 'large.bin', input);
    await chooseTarget(page, 'large.bin', 'base64');
    await convert(page, 'large.bin', { timeout: 60_000 });
    const name = outputName('large.bin', ext('bin', 'base64'));
    const d = await save(page, ctx, 'large.bin', name);
    assert.ok(Buffer.from(d.bytes.toString('latin1'), 'base64').equals(input));
    await files.remove();
  }, { timeout: 120_000 });

  e2e('the options form is rendered from the core schema', async ({ ctx }) => {
    const { files, page } = await addOne(ctx, 'notes.txt', TEXT);
    const source = detect(Buffer.from(TEXT), 'notes.txt');
    // A target with both basic and advanced options shows both, in order.
    const target = core.targets(source).find((t) => {
      const s = core.optionsSchema(source, t.id);
      return s.some((o) => o.category === 'basic') && s.some((o) => o.category === 'advanced');
    }) ?? core.targets(source).find((t) => core.optionsSchema(source, t.id).length);
    await chooseTarget(page, 'notes.txt', target.id);
    const schema = core.optionsSchema(source, target.id).filter((o) => !APP_MANAGED_KEYS.has(o.key) && o.key !== 'preset');
    await openAdvanced(page, 'notes.txt');
    const labels = await optionLabels(page, 'notes.txt');
    const ordered = [...schema.filter((o) => o.category !== 'advanced'), ...schema.filter((o) => o.category === 'advanced')];
    assert.deepEqual(labels, ordered.map((o) => o.label));
    for (const o of schema) {
      const id = await controlId(page, 'notes.txt', o.label);
      const info = await page.withElement(`#${id}`, (el) => ({ tag: el.tagName, type: el.type, options: el.tagName === 'SELECT' ? [...el.options].map((x) => x.value) : null, min: el.min, max: el.max }));
      if (o.type === 'bool') assert.equal(info.type, 'checkbox', o.key);
      if (o.type === 'enum') assert.deepEqual(info.options, o.choices, o.key);
      if (o.type === 'int' && o.min !== null && o.max !== null) {
        assert.equal(Number(info.min), o.min, o.key);
        assert.equal(Number(info.max), o.max, o.key);
      }
    }
    if (schema.some((o) => o.category === 'advanced') && schema.some((o) => o.category !== 'advanced')) {
      assert.equal(await page.withElement({ card: 'notes.txt', sel: '.field-group-label' }, (el) => el.textContent), 'More');
    }
    await files.remove();
  });

  e2e('Base64 line width (typed from the keyboard) changes the output', async ({ ctx }) => {
    const input = Buffer.from('wrap me '.repeat(60));
    const { files, page } = await addOne(ctx, 'wrap.txt', input);
    const source = detect(input, 'wrap.txt');
    const field = core.optionsSchema(source, 'base64').find((o) => o.key === 'line_width');
    await chooseTarget(page, 'wrap.txt', 'base64');
    await convert(page, 'wrap.txt');
    const name = outputName('wrap.txt', ext(source, 'base64'));
    const wrapped = (await save(page, ctx, 'wrap.txt', name)).bytes.toString('latin1');
    const lines = wrapped.trimEnd().split('\n');
    assert.ok(lines.length > 1 && lines[0].length === field.default, `wrapped at ${field.default}`);

    // "Convert again", open the options, type 0 into the line width.
    await page.click({ card: 'wrap.txt', sel: tid('again') });
    await waitForCard(page, 'wrap.txt', 'ready');
    await openAdvanced(page, 'wrap.txt');
    const { id } = await control(page, 'wrap.txt', field.label);
    await page.focus(`#${id}`);
    for (let i = 0; i < String(field.default).length; i += 1) {
      await page.press('Backspace');
      await page.press('Delete');
    }
    await page.type('0');
    await page.press('Tab'); // commits the value (change event)
    await waitForControl(page, 'wrap.txt', field.label, { value: '0' });
    await convert(page, 'wrap.txt');
    const single = (await save(page, ctx, 'wrap.txt', name)).bytes.toString('latin1');
    assert.equal(single.trimEnd().includes('\n'), false, 'a single unbroken line');
    assert.equal(single.replace(/\s+/g, ''), wrapped.replace(/\s+/g, ''), 'same content otherwise');
    assert.ok(Buffer.from(single, 'base64').equals(input));
    await files.remove();
  });

  e2e('the hex "uppercase" switch changes the output', async ({ ctx }) => {
    const input = allBytes();
    const { files, page } = await addOne(ctx, 'upper.bin', input);
    await chooseTarget(page, 'upper.bin', 'hex');
    const field = core.optionsSchema('bin', 'hex').find((o) => o.key === 'uppercase');
    await openAdvanced(page, 'upper.bin');
    const { id, checked } = await control(page, 'upper.bin', field.label);
    assert.equal(checked, false);
    await page.click(`#${id}`);
    await waitForControl(page, 'upper.bin', field.label, { checked: true });
    await convert(page, 'upper.bin');
    const out = (await save(page, ctx, 'upper.bin', outputName('upper.bin', ext('bin', 'hex')))).bytes.toString('latin1').replace(/\s+/g, '');
    assert.equal(out, input.toString('hex').toUpperCase());
    await files.remove();
  });

  e2e('text re-encoding to UTF-16LE with a byte order mark', async ({ ctx }) => {
    const { files, page } = await addOne(ctx, 'unicode.txt', TEXT);
    const source = detect(Buffer.from(TEXT), 'unicode.txt');
    const schema = core.optionsSchema(source, source);
    const encoding = schema.find((o) => o.key === 'encoding');
    const bom = schema.find((o) => o.key === 'bom');
    assert.ok(encoding && encoding.choices.includes('utf-16le') && bom, 'the core offers UTF-16LE with a BOM');
    await chooseTarget(page, 'unicode.txt', source);
    await setOption(page, 'unicode.txt', encoding.label, 'utf-16le');
    await waitForControl(page, 'unicode.txt', encoding.label, { value: 'utf-16le' });
    await page.click(`#${(await control(page, 'unicode.txt', bom.label)).id}`);
    await waitForControl(page, 'unicode.txt', bom.label, { checked: true });
    await convert(page, 'unicode.txt');
    const d = await save(page, ctx, 'unicode.txt', outputName('unicode.txt', ext(source, source)));
    assert.deepEqual([...d.bytes.subarray(0, 2)], [0xff, 0xfe], 'UTF-16LE byte order mark');
    assert.equal(new TextDecoder('utf-16le').decode(d.bytes.subarray(2)), TEXT);
    await files.remove();
  });

  e2e('Save uses the native save dialog where the browser has one', async ({ ctx }) => {
    const { files, page } = await addOne(ctx, 'dialog.txt', TEXT, { fsAccess: true });
    // The test answers the dialog: it records the suggested name and the bytes written.
    await page.evaluate(() => {
      window.__saved = null;
      window.showSaveFilePicker = async (opts) => ({
        createWritable: async () => {
          const parts = [];
          return {
            write: async (b) => parts.push(b),
            close: async () => {
              const bytes = new Uint8Array(await new Blob(parts).arrayBuffer());
              window.__saved = { name: opts.suggestedName, accept: opts.types?.[0]?.accept ?? null, bytes: Array.from(bytes) };
            },
          };
        },
      });
    });
    await chooseTarget(page, 'dialog.txt', 'base64');
    await convert(page, 'dialog.txt');
    const before = ctx.downloadsSoFar().size;
    await page.click({ card: 'dialog.txt', sel: tid('save') });
    const saved = await page.waitFor(() => window.__saved, [], { message: 'the save dialog to be used' });
    const name = outputName('dialog.txt', ext(detect(Buffer.from(TEXT), 'dialog.txt'), 'base64'));
    assert.equal(saved.name, name);
    assert.ok(saved.accept && Object.values(saved.accept)[0].includes(`.${name.split('.').pop()}`), 'the dialog filters by the extension');
    assert.equal(Buffer.from(Buffer.from(saved.bytes).toString('latin1'), 'base64').toString(), TEXT);
    await page.waitForText(`Saved ${name}`, { selector: '#toasts' });
    assert.equal(ctx.downloadsSoFar().size, before, 'no download as well');
    await files.remove();
  });

  const presetCase = (() => {
    const inputs = { txt: Buffer.from(TEXT), bin: pseudoRandom(4096) };
    for (const [source, data] of Object.entries(inputs)) {
      for (const t of core?.targets(source) ?? []) {
        const p = core.optionsSchema(source, t.id).find((o) => o.key === 'preset' && o.type === 'enum');
        if (p && ['high', 'small'].every((c) => p.choices.includes(c))) return { source, target: t.id, data };
      }
    }
    return null;
  })();

  e2e('the quality preset control changes the output', async ({ ctx }) => {
    const { source, target, data } = presetCase;
    const name = `preset.${source}`;
    const { files, page } = await addOne(ctx, name, data);
    await chooseTarget(page, name, target);
    await page.waitForSelector({ card: name, sel: tid('preset') });
    assert.equal(await page.withElement({ card: name, sel: tid('preset-balanced') }, (el) => el.checked), true, 'the default preset is preselected');
    const out = {};
    for (const preset of ['high', 'small']) {
      await page.click({ card: name, sel: tid(`preset-${preset}`) });
      await page.waitFor((n, p) => [...document.querySelectorAll('[data-testid="file-card"]')].find((li) => li.querySelector('.file-name')?.textContent === n)?.querySelector(`[data-testid="preset-${p}"]`)?.checked, [name, preset]);
      await convert(page, name);
      out[preset] = (await save(page, ctx, name, outputName(name, ext(source, target)))).bytes;
      await page.click({ card: name, sel: tid('again') });
      await waitForCard(page, name, 'ready');
    }
    assert.ok(out.small.length <= out.high.length, 'Small is not larger than High');
    await files.remove();
  }, { skip: needsFormats(presetCase, 'no generated input has a target with a quality preset') });
});

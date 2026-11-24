// Convertible web app - one queue card per file.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { State } from '../model/queue.js';
import { formatBytes, formatLabel, percentText, sizeChange } from '../model/format.js';
import { categoryKey } from '../model/targets.js';
import { valueOf } from '../model/options.js';
import { clear, h, icon, put, uniqueId } from './dom.js';
import { createProgressMark } from './motion.js';

export function badge(formatId, category, { shape = 'round', detected = true } = {}) {
  return h(
    'span',
    {
      class: ['badge', shape === 'square' ? 'badge-square' : 'badge-round', detected ? null : 'badge-unknown'],
      'data-cat': categoryKey(category),
      title: detected ? null : 'Format not recognised; treated as raw data',
    },
    h('span', { class: 'badge-dot', 'aria-hidden': 'true' }),
    detected ? formatLabel(formatId) : 'FILE',
  );
}

function button(label, { iconName = null, variant = 'secondary', testid = null, onClick, ariaLabel = null, focusKey = null, extra = {} } = {}) {
  return h(
    'button',
    {
      type: 'button',
      class: ['btn', `btn-${variant}`],
      'data-testid': testid,
      'aria-label': ariaLabel,
      'data-focus': focusKey,
      onClick,
      ...extra,
    },
    iconName ? icon(iconName, { size: 18 }) : null,
    h('span', {}, label),
  );
}

function presetControl(item, model, settings, actions) {
  const name = uniqueId('preset');
  const current = item.options.preset ?? settings.preset ?? model.preset.default;
  return h(
    'fieldset',
    { class: 'segmented', 'data-testid': 'preset' },
    h('legend', { class: 'field-label' }, 'Quality'),
    h(
      'div',
      { class: 'segmented-track' },
      model.preset.choices.map((c) =>
        h(
          'label',
          { class: 'segment' },
          h('input', {
            type: 'radio',
            name,
            value: c.value,
            checked: c.value === current,
            'data-focus': `preset-${c.value}`,
            'data-testid': `preset-${c.value}`,
            onChange: () => actions.setOptions(item.key, { preset: c.value }),
          }),
          h('span', {}, c.label),
        ),
      ),
    ),
  );
}

function controlElement(item, control, actions) {
  const id = uniqueId(`opt-${control.key}`);
  const value = valueOf(control, item.options);
  const describe = control.description ? uniqueId('desc') : null;
  const set = (v) => actions.setOptions(item.key, { [control.key]: v });
  const help = control.description ? h('p', { class: 'field-help', id: describe }, control.description) : null;
  let input;
  switch (control.kind) {
    case 'slider': {
      const out = h('output', { class: 'field-value', for: id }, String(value));
      input = h('input', {
        id,
        type: 'range',
        min: control.min,
        max: control.max,
        step: control.step,
        value,
        'aria-describedby': describe,
        'data-focus': `opt-${control.key}`,
        onInput: (e) => {
          out.textContent = e.target.value;
        },
        onChange: (e) => set(control.type === 'int' ? Number.parseInt(e.target.value, 10) : Number.parseFloat(e.target.value)),
      });
      return h('div', { class: 'field field-slider' }, h('div', { class: 'field-row' }, h('label', { for: id, class: 'field-label' }, control.label), out), input, help);
    }
    case 'stepper':
      input = h('input', {
        id,
        type: 'number',
        inputmode: control.type === 'int' ? 'numeric' : 'decimal',
        min: control.min ?? null,
        max: control.max ?? null,
        step: control.step,
        value,
        'aria-describedby': describe,
        'data-focus': `opt-${control.key}`,
        onChange: (e) => set(e.target.value === '' ? undefined : Number(e.target.value)),
      });
      return h('div', { class: 'field field-inline' }, h('label', { for: id, class: 'field-label' }, control.label), input, help);
    case 'switch':
      input = h('input', {
        id,
        type: 'checkbox',
        role: 'switch',
        class: 'switch',
        checked: Boolean(value),
        'aria-describedby': describe,
        'data-focus': `opt-${control.key}`,
        onChange: (e) => set(e.target.checked),
      });
      return h('div', { class: 'field field-switch' }, h('label', { for: id, class: 'field-label' }, control.label), input, help);
    case 'menu':
      input = h(
        'select',
        { id, 'aria-describedby': describe, 'data-focus': `opt-${control.key}`, onChange: (e) => set(e.target.value) },
        control.choices.map((c) => h('option', { value: c.value, selected: c.value === value }, c.label)),
      );
      return h('div', { class: 'field field-inline' }, h('label', { for: id, class: 'field-label' }, control.label), h('div', { class: 'select-wrap' }, input, icon('chevron', { size: 16 })), help);
    default:
      input = h('input', {
        id,
        type: 'text',
        value: value ?? '',
        spellcheck: 'false',
        'aria-describedby': describe,
        'data-focus': `opt-${control.key}`,
        onChange: (e) => set(e.target.value),
      });
      return h('div', { class: 'field field-inline' }, h('label', { for: id, class: 'field-label' }, control.label), input, help);
  }
}

function optionsSection(item, model, settings, actions, openState) {
  if (!model || model.empty) return null;
  const parts = [];
  if (model.preset) parts.push(presetControl(item, model, settings, actions));
  const basic = model.basic.map((c) => controlElement(item, c, actions));
  const advanced = model.advanced.map((c) => controlElement(item, c, actions));
  const managed = model.managed.length
    ? h('p', { class: 'field-note' }, icon('lock', { size: 16 }), settings.stripMetadata ? 'Metadata is removed (change in Settings).' : 'Metadata is kept (change in Settings).')
    : null;
  const hasMore = basic.length || advanced.length;
  if (hasMore) {
    const details = h(
      'details',
      { class: 'advanced', 'data-testid': 'advanced', open: openState.get(item.key) || false },
      h('summary', { 'data-focus': 'advanced' }, icon('settings', { size: 18 }), h('span', {}, model.preset ? 'Advanced' : 'Options'), icon('chevron', { size: 16 })),
      h('div', { class: 'advanced-body' }, basic, advanced.length && basic.length ? h('p', { class: 'field-group-label' }, 'More') : null, advanced),
    );
    details.addEventListener('toggle', () => openState.set(item.key, details.open));
    parts.push(details);
  }
  // The metadata note reflects an app-wide setting: always visible, never
  // hidden inside the collapsed options.
  if (managed) parts.push(managed);
  return h('div', { class: 'card-options' }, parts);
}

/**
 * Re-renders a card's content for the item's current state.
 * `record` keeps per-card objects across renders (the progress mark).
 */
export function renderCard(li, record, item, deps) {
  const { formats, settings, actions, targetsFor, schemaFor, openState } = deps;
  const focused = li.contains(document.activeElement) ? document.activeElement.dataset.focus : null;
  li.dataset.state = item.state;
  li.setAttribute('aria-busy', item.state === State.DETECTING || item.state === State.CONVERTING ? 'true' : 'false');
  clear(li);

  const sourceInfo = item.source ? formats.get(item.source) : null;
  const nameId = `${item.key}-name`;
  li.setAttribute('aria-labelledby', nameId);

  const head = h(
    'div',
    { class: 'card-head' },
    item.source ? badge(item.source, item.category ?? sourceInfo?.category, { detected: item.sourceDetected }) : h('span', { class: 'badge badge-pending' }, h('span', { class: 'badge-dot' }), '···'),
    h(
      'div',
      { class: 'file-meta' },
      h('h3', { class: 'file-name', id: nameId, title: item.name }, item.name),
      h('p', { class: 'file-sub' }, formatBytes(item.size), item.source && sourceInfo ? h('span', { class: 'file-kind' }, ` · ${sourceInfo.name}`) : null),
    ),
    item.state === State.CONVERTING || item.state === State.QUEUED
      ? null
      : h('button', { type: 'button', class: 'icon-btn', 'aria-label': `Remove ${item.name}`, 'data-testid': 'remove', 'data-focus': 'remove', onClick: () => actions.remove(item.key) }, icon('close', { size: 18 })),
  );
  li.append(head);

  const body = h('div', { class: 'card-body' });
  li.append(body);

  const targets = item.source ? targetsFor(item.source) : null;
  const target = targets && item.target ? targets.find((t) => t.id === item.target) : null;

  if (item.state === State.DETECTING) {
    put(body, h('p', { class: 'card-status' }, 'Identifying format…'));
  } else if (item.state === State.READY || item.state === State.CANCELLED) {
    if (!targets || !targets.length) {
      put(body, h('p', { class: 'card-status' }, targets ? 'No conversions are available for this file yet.' : 'Loading formats…'));
    } else {
      const compressible = targets.some((t) => t.compress || t.category === 'archive');
      put(body, 
        h(
          'div',
          { class: 'convert-row' },
          h('span', { class: 'to-label', id: `${item.key}-to` }, 'to'),
          h(
            'button',
            {
              type: 'button',
              class: 'target-btn',
              'aria-haspopup': 'dialog',
              'aria-describedby': `${item.key}-to`,
              'data-testid': 'target-picker',
              'data-focus': 'target',
              onClick: () => actions.pickTarget(item.key),
            },
            target ? badge(target.id, target.category, { shape: 'square' }) : h('span', { class: 'badge badge-pending' }, 'Choose'),
            h('span', { class: 'target-name' }, target ? (target.compress ? `${target.name}, compressed` : target.name) : 'Choose a format'),
            icon('chevron', { size: 18 }),
          ),
        ),
        optionsSection(item, target ? schemaFor(item.source, target.id) : null, settings, actions, openState),
        h(
          'div',
          { class: 'card-actions' },
          compressible ? button('Compress', { iconName: 'compress', variant: 'ghost', testid: 'compress', focusKey: 'compress', onClick: () => actions.compress(item.key) }) : null,
          button('Convert', { iconName: 'arrow', variant: 'primary', testid: 'convert', focusKey: 'convert', onClick: () => actions.convert(item.key), extra: { disabled: !target } }),
        ),
      );
      if (item.state === State.CANCELLED) body.prepend(h('p', { class: 'card-status card-status-muted' }, 'Cancelled.'));
    }
  } else if (item.state === State.QUEUED || item.state === State.CONVERTING) {
    if (!record.mark) record.mark = createProgressMark({ size: 44 });
    const mark = record.mark;
    if (item.state === State.QUEUED) mark.reset();
    const pct = h('span', { class: 'progress-pct', 'data-role': 'pct' }, item.state === State.QUEUED ? '' : percentText(item.progress));
    const stage = h('span', { class: 'progress-stage', 'data-role': 'stage' }, item.state === State.QUEUED ? 'Waiting…' : `${item.stage || 'Converting'}…`);
    const bar = h('div', {
      class: 'progress-wrap',
      role: 'progressbar',
      'aria-label': `Converting ${item.name}`,
      'aria-valuemin': 0,
      'aria-valuemax': 100,
      'aria-valuenow': Math.floor(item.progress * 100),
      'aria-valuetext': item.state === State.QUEUED ? 'Waiting' : `${percentText(item.progress)}, ${item.stage}`,
      'data-role': 'bar',
    });
    bar.append(mark.el, h('div', { class: 'progress-text' }, h('p', { class: 'progress-title' }, 'To ', target ? formatLabel(target.id) : '…'), h('p', {}, stage, ' ', pct)));
    put(body, 
      h(
        'div',
        { class: 'progress-row' },
        bar,
        button('Cancel', { iconName: 'stop', variant: 'ghost', testid: 'cancel', focusKey: 'cancel', ariaLabel: `Cancel ${item.name}`, onClick: () => actions.cancel(item.key) }),
      ),
    );
    if (item.state === State.CONVERTING && item.progress <= 0) mark.spin();
    else if (item.state === State.CONVERTING) mark.set(item.progress);
  } else if (item.state === State.DONE) {
    const r = item.result;
    const change = sizeChange(item.size, r.size);
    if (!record.mark) record.mark = createProgressMark({ size: 44 });
    const wasRunning = record.mark.value < 1;
    if (wasRunning) record.mark.complete();
    const warnings = r.warnings && r.warnings.length
      ? h(
          'details',
          { class: 'warnings', 'data-testid': 'warnings' },
          h('summary', {}, icon('info', { size: 16 }), r.warnings.length === 1 ? '1 note' : `${r.warnings.length} notes`),
          h('ul', {}, r.warnings.map((w) => h('li', {}, w))),
        )
      : null;
    put(body, 
      h(
        'div',
        { class: 'result' },
        record.mark.el,
        h(
          'div',
          { class: 'result-text' },
          h('p', { class: 'result-name', title: r.name }, r.name),
          h(
            'p',
            { class: 'result-sizes', 'data-testid': 'result-sizes' },
            h('span', {}, formatBytes(item.size)),
            icon('arrow', { size: 14 }),
            h('span', { class: 'result-after' }, formatBytes(r.size)),
            change.label ? h('span', { class: ['saving', change.smaller ? 'saving-good' : 'saving-neutral'] }, change.label) : null,
          ),
        ),
      ),
      warnings,
      h(
        'div',
        { class: 'card-actions card-actions-result' },
        button('Save', { iconName: 'download', variant: 'primary', testid: 'save', focusKey: 'save', ariaLabel: `Save ${r.name}`, onClick: () => actions.save(item.key) }),
        deps.canShare(r) ? button('Share', { iconName: 'share', variant: 'ghost', testid: 'share', focusKey: 'share', onClick: () => actions.share(item.key) }) : null,
        deps.canOpen(r) ? button('Open', { iconName: 'open', variant: 'ghost', testid: 'open', focusKey: 'open', onClick: () => actions.open(item.key) }) : null,
        button('Convert again', { iconName: 'again', variant: 'ghost', testid: 'again', focusKey: 'again', onClick: () => actions.again(item.key) }),
      ),
    );
  } else if (item.state === State.FAILED) {
    const e = item.error || {};
    put(body, 
      h(
        'div',
        { class: 'error-box', role: 'alert', 'data-testid': 'error' },
        icon('alert', { size: 20 }),
        h('div', {}, h('p', { class: 'error-title' }, e.title || 'Something went wrong'), e.message ? h('p', { class: 'error-message' }, e.message) : null),
      ),
      e.permanent
        ? null
        : h(
            'div',
            { class: 'card-actions' },
            targets && targets.length ? button('Change format', { variant: 'ghost', testid: 'change-format', focusKey: 'target', onClick: () => actions.pickTarget(item.key) }) : null,
            button('Try again', { iconName: 'again', variant: 'secondary', testid: 'retry', focusKey: 'retry', onClick: () => actions.convert(item.key) }),
          ),
    );
  }

  if (focused) {
    const again = li.querySelector(`[data-focus="${CSS.escape(focused)}"]`);
    if (again) again.focus({ preventScroll: true });
    else li.querySelector('button, [href], input, select, summary')?.focus({ preventScroll: true });
  }
}

/** Updates only the progress parts of a converting card (no re-render). */
export function updateProgress(li, record, item) {
  const pct = li.querySelector('[data-role="pct"]');
  const stage = li.querySelector('[data-role="stage"]');
  const bar = li.querySelector('[data-role="bar"]');
  if (pct) pct.textContent = percentText(item.progress);
  if (stage) stage.textContent = `${item.stage || 'Converting'}…`;
  if (bar) {
    bar.setAttribute('aria-valuenow', Math.floor(item.progress * 100));
    bar.setAttribute('aria-valuetext', `${percentText(item.progress)}, ${item.stage}`);
  }
  if (record.mark) {
    if (item.progress > 0) record.mark.set(item.progress);
    else record.mark.spin();
  }
}

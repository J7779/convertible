// Convertible web app - tiny DOM helpers and the icon set (no framework).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

const SVG_NS = 'http://www.w3.org/2000/svg';

function apply(el, attrs) {
  for (const [key, value] of Object.entries(attrs || {})) {
    if (value === undefined || value === null || value === false) continue;
    if (key === 'class') el.setAttribute('class', Array.isArray(value) ? value.filter(Boolean).join(' ') : value);
    else if (key === 'dataset') Object.assign(el.dataset, value);
    else if (key === 'style') for (const [p, v] of Object.entries(value)) el.style.setProperty(p, v);
    else if (key.startsWith('on') && typeof value === 'function') el.addEventListener(key.slice(2).toLowerCase(), value);
    else if (key === 'text') el.textContent = value;
    else if (value === true) el.setAttribute(key, '');
    else el.setAttribute(key, String(value));
  }
}

function append(el, children) {
  for (const child of children.flat(Infinity)) {
    if (child === null || child === undefined || child === false) continue;
    el.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
}

/** Appends children to an existing element, skipping null/false like h() does. */
export function put(el, ...children) {
  append(el, children);
  return el;
}

/** h('button', {class: 'btn', onClick}, 'Label') */
export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  apply(el, attrs);
  append(el, children);
  return el;
}

/** SVG element with attributes. */
export function s(tag, attrs = {}, ...children) {
  const el = document.createElementNS(SVG_NS, tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (value === undefined || value === null || value === false) continue;
    if (key === 'class') el.setAttribute('class', value);
    else if (key === 'style') for (const [p, v] of Object.entries(value)) el.style.setProperty(p, v);
    else el.setAttribute(key, String(value));
  }
  append(el, children);
  return el;
}

// 24 px stroke icons (1.75 px stroke, round joins).
const ICONS = {
  lock: ['M6 11h12v9H6z', 'M8.5 11V8a3.5 3.5 0 0 1 7 0v3'],
  arrow: ['M5 12h14', 'M13 6l6 6-6 6'],
  chevron: ['M6 9l6 6 6-6'],
  close: ['M6 6l12 12', 'M18 6L6 18'],
  download: ['M12 4v11', 'M7 10l5 5 5-5', 'M5 20h14'],
  share: ['M12 15V4', 'M8 8l4-4 4 4', 'M6 12v7h12v-7'],
  open: ['M14 5h5v5', 'M19 5l-8 8', 'M17 14v5H5V7h5'],
  again: ['M4 12a8 8 0 1 0 2.4-5.7', 'M4 4v5h5'],
  settings: ['M4 7h10', 'M18 7h2', 'M4 17h4', 'M12 17h8', 'M16 5v4', 'M10 15v4'],
  grid: ['M4 4h7v7H4z', 'M13 4h7v7h-7z', 'M4 13h7v7H4z', 'M13 13h7v7h-7z'],
  search: ['M10.5 17a6.5 6.5 0 1 0 0-13 6.5 6.5 0 0 0 0 13z', 'M15.5 15.5L20 20'],
  check: ['M5 12.5l4.5 4.5L19 7.5'],
  alert: ['M12 8v5', 'M12 16.5v.5', 'M10.3 4.3L2.8 18a2 2 0 0 0 1.7 3h15a2 2 0 0 0 1.7-3L13.7 4.3a2 2 0 0 0-3.4 0z'],
  folder: ['M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z'],
  plus: ['M12 5v14', 'M5 12h14'],
  compress: ['M9 4v5H4', 'M15 4v5h5', 'M9 20v-5H4', 'M15 20v-5h5'],
  install: ['M12 3v12', 'M7 10l5 5 5-5', 'M4 15v4a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-4'],
  stop: ['M7 7h10v10H7z'],
  info: ['M12 11v6', 'M12 7.5v.5', 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z'],
  trash: ['M4 7h16', 'M9 7V4h6v3', 'M6 7l1 13h10l1-13'],
};

export function icon(name, { size = 20, label = null } = {}) {
  const el = s(
    'svg',
    {
      class: `icon icon-${name}`,
      viewBox: '0 0 24 24',
      width: size,
      height: size,
      fill: 'none',
      stroke: 'currentColor',
      'stroke-width': 1.75,
      'stroke-linecap': 'round',
      'stroke-linejoin': 'round',
      'aria-hidden': label ? null : 'true',
      role: label ? 'img' : null,
      'aria-label': label,
      focusable: 'false',
    },
    ...(ICONS[name] || []).map((d) => s('path', { d })),
  );
  return el;
}

let uid = 0;
export function uniqueId(prefix = 'cv') {
  uid += 1;
  return `${prefix}-${uid}`;
}

/** Polite screen-reader announcement. */
export function announce(text) {
  const region = document.getElementById('announcer');
  if (!region) return;
  region.textContent = '';
  // A separate task so repeated identical messages are still announced.
  setTimeout(() => {
    region.textContent = text;
  }, 30);
}

export function clear(el) {
  while (el.firstChild) el.firstChild.remove();
}

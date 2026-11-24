// Convertible web app - the brand mark in motion: startup intro, spinner and
// determinate progress, built as inline SVG and driven by Web Animations from
// the storyboard in assets/brand/animation.json.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import storyboard from '../generated/animation.js';
import { s, uniqueId } from './dom.js';
import { sectionAnimations, trimCss } from './motion-model.js';

const CENTERLINE = storyboard.geometry.centerline.d;
const SEED = storyboard.geometry.seed;
const SQUARE = storyboard.geometry.endSquare;
const OUTLINE = storyboard.geometry.markOutline.d;

export function prefersReducedMotion() {
  return typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches;
}

function fluxGradient(id) {
  const paint = storyboard.paints.flux;
  return s(
    'linearGradient',
    { id, gradientUnits: 'userSpaceOnUse', x1: paint.x1, y1: paint.y1, x2: paint.x2, y2: paint.y2, class: 'cv-flux' },
    ...paint.stops.map((stop, i) => s('stop', { offset: stop.offset, class: `cv-flux-stop cv-flux-stop-${i}` })),
  );
}

/** The static logo (filled outline), for headers and empty states. */
export function staticMark({ size = 32, label = null, className = '' } = {}) {
  const id = uniqueId('cv-flux');
  return s(
    'svg',
    {
      class: `mark ${className}`,
      viewBox: '120 120 784 784',
      width: size,
      height: size,
      role: label ? 'img' : null,
      'aria-label': label,
      'aria-hidden': label ? null : 'true',
      focusable: 'false',
    },
    s('defs', {}, fluxGradient(id)),
    s('path', { d: OUTLINE, fill: `url(#${id})` }),
  );
}

function layeredMark(id, { ripples = false, ghost = false, track = false } = {}) {
  const gradient = `url(#${id})`;
  const layers = {};
  const svg = s('svg', { viewBox: '0 0 1024 1024', 'aria-hidden': 'true', focusable: 'false', class: 'mark-anim' }, s('defs', {}, fluxGradient(id)));
  const stage = s('g', { class: 'mk-stage', style: { 'transform-origin': '512px 512px' } });
  svg.append(stage);
  layers.stage = stage;
  if (track) {
    layers.track = s(
      'g',
      { class: 'mk-track' },
      s('path', { d: CENTERLINE, class: 'mk-stroke', stroke: gradient }),
      s('circle', { cx: SEED.cx, cy: SEED.cy, r: SEED.r, fill: gradient }),
    );
    stage.append(layers.track);
  }
  if (ripples) {
    layers.rippleRound = s('circle', { cx: SEED.cx, cy: SEED.cy, r: SEED.r, class: 'mk-ripple-round', style: { 'transform-origin': `${SEED.cx}px ${SEED.cy}px`, opacity: '0' } });
    stage.append(layers.rippleRound);
  }
  layers.rippleSquare = s('rect', {
    x: SQUARE.x,
    y: SQUARE.y,
    width: SQUARE.width,
    height: SQUARE.height,
    class: 'mk-ripple-square',
    style: { 'transform-origin': '764px 764px', opacity: '0' },
  });
  stage.append(layers.rippleSquare);
  if (ghost) {
    layers.ghost = s('path', { d: CENTERLINE, pathLength: 1000, class: 'mk-stroke', stroke: gradient, style: { opacity: '0' } });
    stage.append(layers.ghost);
  }
  layers.trace = s('path', { d: CENTERLINE, pathLength: 1000, class: 'mk-stroke mk-runner', stroke: gradient });
  layers.runner = layers.trace;
  stage.append(layers.trace);
  layers.seed = s('circle', { cx: SEED.cx, cy: SEED.cy, r: SEED.r, fill: gradient, class: 'mk-seed', style: { 'transform-origin': `${SEED.cx}px ${SEED.cy}px` } });
  stage.append(layers.seed);
  return { svg, layers };
}

function animateSection(layers, section, options) {
  const animations = [];
  for (const { layer, keyframes } of sectionAnimations(storyboard, section)) {
    const el = layers[layer];
    if (!el) continue;
    animations.push(el.animate(keyframes, { duration: storyboard[section].durationMs, fill: 'both', ...options }));
  }
  return animations;
}

/**
 * Plays the startup intro in `host` (an element that is shown full screen).
 * Resolves when it finished or was skipped (tap, click or any key).
 */
export function playIntro(host) {
  return new Promise((resolve) => {
    const { svg, layers } = layeredMark(uniqueId('cv-intro'), { ripples: true, ghost: true });
    svg.classList.add('intro-mark');
    host.append(svg);
    let done = false;
    let animations = [];
    const finish = () => {
      if (done) return;
      done = true;
      for (const a of animations) a.finish();
      window.removeEventListener('keydown', finish, true);
      host.removeEventListener('pointerdown', finish);
      const fade = host.animate([{ opacity: 1 }, { opacity: 0 }], {
        duration: prefersReducedMotion() ? 140 : 360,
        easing: 'cubic-bezier(0.16, 1, 0.3, 1)',
        fill: 'forwards',
      });
      let closed = false;
      const close = () => {
        if (closed) return;
        closed = true;
        host.remove();
        resolve();
      };
      fade.finished.then(close, close);
      setTimeout(close, 600); // background tabs may not run animations
    };
    window.addEventListener('keydown', finish, true);
    host.addEventListener('pointerdown', finish);
    if (prefersReducedMotion() || typeof svg.animate !== 'function') {
      // Static mark: a short fade in, a beat, then out.
      Object.assign(layers.trace.style, trimCss(0, 1));
      svg.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 140, fill: 'both' });
      setTimeout(finish, 700);
      return;
    }
    animations = animateSection(layers, 'intro', {});
    Promise.all(animations.map((a) => a.finished)).then(finish, finish);
    setTimeout(finish, storyboard.intro.durationMs + 400);
  });
}

/**
 * The per-file progress indicator. Determinate progress trims the stroke to
 * the fraction (finished = the logo); while the fraction is unknown it runs
 * the looping spinner.
 */
export function createProgressMark({ size = 44 } = {}) {
  const { svg, layers } = layeredMark(uniqueId('cv-prog'), { track: true });
  svg.classList.add('progress-mark');
  svg.setAttribute('width', size);
  svg.setAttribute('height', size);
  svg.setAttribute('viewBox', '120 120 784 784');
  let spinning = [];
  let value = 0;

  const setTrim = (p) => {
    const t = trimCss(0, p);
    layers.runner.style.strokeDasharray = t.strokeDasharray;
    layers.runner.style.strokeDashoffset = t.strokeDashoffset;
    layers.seed.style.scale = p > 0 ? '1' : '0';
  };

  const stopSpinner = () => {
    for (const a of spinning) a.cancel();
    spinning = [];
    svg.classList.remove('is-spinning');
  };

  setTrim(0);
  return {
    el: svg,
    get value() {
      return value;
    },
    spin() {
      if (spinning.length) return;
      svg.classList.add('is-spinning');
      if (prefersReducedMotion()) {
        setTrim(1);
        spinning = [
          svg.animate([{ opacity: 1 }, { opacity: 0.45 }, { opacity: 1 }], {
            duration: 1600,
            iterations: Infinity,
            easing: 'cubic-bezier(0.65, 0, 0.35, 1)',
          }),
        ];
        return;
      }
      spinning = animateSection(layers, 'spinner', { iterations: Infinity });
    },
    set(p) {
      const next = Math.max(value, Math.min(1, p));
      if (spinning.length) stopSpinner();
      value = next;
      setTrim(next);
    },
    complete() {
      stopSpinner();
      value = 1;
      setTrim(1);
      if (!prefersReducedMotion() && typeof layers.rippleSquare.animate === 'function') {
        layers.rippleSquare.animate(
          [
            { scale: '1', opacity: 0, easing: 'cubic-bezier(0.33, 1, 0.68, 1)' },
            { scale: '1.25', opacity: 0.45, offset: 0.1, easing: 'cubic-bezier(0.16, 1, 0.3, 1)' },
            { scale: '1.6', opacity: 0 },
          ],
          { duration: 480 },
        );
      }
    },
    reset() {
      stopSpinner();
      value = 0;
      setTrim(0);
    },
  };
}

// Convertible web app - turns the brand motion storyboard (animation.json)
// into Web Animations keyframes. Pure, no DOM (tested in Node).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

/** Evaluates a CSS cubic-bezier(x1, y1, x2, y2) timing function at progress x. */
export function cubicBezier([x1, y1, x2, y2]) {
  const cx = 3 * x1;
  const bx = 3 * (x2 - x1) - cx;
  const ax = 1 - cx - bx;
  const cy = 3 * y1;
  const by = 3 * (y2 - y1) - cy;
  const ay = 1 - cy - by;
  const sampleX = (t) => ((ax * t + bx) * t + cx) * t;
  const sampleY = (t) => ((ay * t + by) * t + cy) * t;
  const slopeX = (t) => (3 * ax * t + 2 * bx) * t + cx;
  return (x) => {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    let t = x;
    for (let i = 0; i < 8; i += 1) {
      const err = sampleX(t) - x;
      if (Math.abs(err) < 1e-6) return sampleY(t);
      const d = slopeX(t);
      if (Math.abs(d) < 1e-6) break;
      t -= err / d;
    }
    let lo = 0;
    let hi = 1;
    t = x;
    for (let i = 0; i < 40; i += 1) {
      const v = sampleX(t);
      if (Math.abs(v - x) < 1e-6) break;
      if (v < x) lo = t;
      else hi = t;
      t = (lo + hi) / 2;
    }
    return sampleY(t);
  };
}

export function easingCss(storyboard, name) {
  const p = name && storyboard.easings[name];
  return p ? `cubic-bezier(${p.join(', ')})` : 'linear';
}

/** Value of a track at time t (ms), honouring holds before/after and per-segment easing. */
export function valueAt(storyboard, track, t) {
  const kfs = track.keyframes;
  if (t <= kfs[0].t) return kfs[0].value;
  for (let i = 0; i < kfs.length - 1; i += 1) {
    const a = kfs[i];
    const b = kfs[i + 1];
    if (t <= b.t) {
      const span = b.t - a.t;
      const x = span > 0 ? (t - a.t) / span : 1;
      const ease = a.easing ? cubicBezier(storyboard.easings[a.easing]) : (v) => v;
      return a.value + (b.value - a.value) * ease(x);
    }
  }
  return kfs[kfs.length - 1].value;
}

const PROPERTY_CSS = {
  scale: (v) => ({ scale: `${round(v)}` }),
  rotationDeg: (v) => ({ rotate: `${round(v)}deg` }),
  opacity: (v) => ({ opacity: round(v) }),
};

function round(v) {
  return Math.round(v * 10000) / 10000;
}

/** Web Animations keyframes for one scalar track (scale, rotationDeg, opacity). */
export function trackKeyframes(storyboard, track, duration) {
  const css = PROPERTY_CSS[track.property];
  if (!css) throw new Error(`unsupported track property ${track.property}`);
  const kfs = track.keyframes;
  const frames = [];
  if (kfs[0].t > 0) frames.push({ offset: 0, ...css(kfs[0].value), easing: 'linear' });
  for (const kf of kfs) frames.push({ offset: round(kf.t / duration), ...css(kf.value), easing: easingCss(storyboard, kf.easing) });
  const last = kfs[kfs.length - 1];
  if (last.t < duration) frames.push({ offset: 1, ...css(last.value) });
  return frames;
}

/** stroke-dasharray/offset for a trim window on a path with pathLength=1000. */
export function trimCss(start, end) {
  const visible = Math.max(0, end - start) * 1000;
  return { strokeDasharray: `${round(visible)} 1000`, strokeDashoffset: `${round(-start * 1000)}` };
}

/**
 * Keyframes for a stroke layer's trimStart/trimEnd tracks combined into one
 * stroke-dasharray/stroke-dashoffset animation. Keyframe times are the union
 * of both tracks; each segment uses the easing of the track that moves in it.
 */
export function trimKeyframes(storyboard, layer, duration) {
  const tracks = Object.fromEntries((layer.tracks || []).filter((t) => t.property === 'trimStart' || t.property === 'trimEnd').map((t) => [t.property, t]));
  const baseStart = layer.base?.trimStart ?? 0;
  const baseEnd = layer.base?.trimEnd ?? 1;
  const times = new Set([0, duration]);
  for (const t of Object.values(tracks)) for (const kf of t.keyframes) times.add(kf.t);
  const sorted = [...times].sort((a, b) => a - b);
  const at = (prop, t, base) => (tracks[prop] ? valueAt(storyboard, tracks[prop], t) : base);
  const segmentEasing = (t0, t1) => {
    let easing = 'linear';
    for (const track of Object.values(tracks)) {
      const kf = track.keyframes.find((k, i) => k.t <= t0 && i < track.keyframes.length - 1 && track.keyframes[i + 1].t >= t1);
      if (kf && valueAt(storyboard, track, t0) !== valueAt(storyboard, track, t1)) easing = easingCss(storyboard, kf.easing);
    }
    return easing;
  };
  return sorted.map((t, i) => ({
    offset: round(t / duration),
    ...trimCss(at('trimStart', t, baseStart), at('trimEnd', t, baseEnd)),
    ...(i < sorted.length - 1 ? { easing: segmentEasing(t, sorted[i + 1]) } : {}),
  }));
}

/**
 * Every animation of a storyboard section ("intro" or "spinner") as
 * [{layer, keyframes, property}] ready for element.animate().
 */
export function sectionAnimations(storyboard, sectionName) {
  const section = storyboard[sectionName];
  const out = [];
  for (const layer of section.layers) {
    const tracks = layer.tracks || [];
    if (tracks.some((t) => t.property === 'trimStart' || t.property === 'trimEnd')) {
      out.push({ layer: layer.id, property: 'trim', keyframes: trimKeyframes(storyboard, layer, section.durationMs) });
    }
    for (const track of tracks) {
      if (track.property === 'trimStart' || track.property === 'trimEnd') continue;
      out.push({ layer: layer.id, property: track.property, keyframes: trackKeyframes(storyboard, track, section.durationMs) });
    }
  }
  return out;
}

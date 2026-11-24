// Convertible web app - the file queue as a small state machine (pure, no DOM).
//
//   detecting --detected--> ready --enqueue--> queued --start--> converting
//   converting --succeed--> done | --fail--> failed | --cancel--> cancelled
//   queued --cancel--> cancelled
//   done | failed | cancelled --reset / setTarget--> ready, --enqueue--> queued
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

export const State = Object.freeze({
  DETECTING: 'detecting',
  READY: 'ready',
  QUEUED: 'queued',
  CONVERTING: 'converting',
  DONE: 'done',
  FAILED: 'failed',
  CANCELLED: 'cancelled',
});

const FINISHED = new Set([State.DONE, State.FAILED, State.CANCELLED]);

/** Allowed events per state. */
const TRANSITIONS = {
  [State.DETECTING]: { detected: State.READY, fail: State.FAILED },
  [State.READY]: { setTarget: State.READY, setOptions: State.READY, enqueue: State.QUEUED, fail: State.FAILED },
  [State.QUEUED]: { start: State.CONVERTING, cancel: State.CANCELLED, fail: State.FAILED },
  [State.CONVERTING]: { progress: State.CONVERTING, succeed: State.DONE, fail: State.FAILED, cancel: State.CANCELLED },
  [State.DONE]: { reset: State.READY, setTarget: State.READY, setOptions: State.READY, enqueue: State.QUEUED },
  [State.FAILED]: { reset: State.READY, setTarget: State.READY, setOptions: State.READY, enqueue: State.QUEUED },
  [State.CANCELLED]: { reset: State.READY, setTarget: State.READY, setOptions: State.READY, enqueue: State.QUEUED },
};

export function canApply(item, type) {
  if (!TRANSITIONS[item.state] || !(type in TRANSITIONS[item.state])) return false;
  if (type === 'enqueue' && !item.target) return false;
  if (type === 'enqueue' && item.error && item.error.permanent) return false;
  return true;
}

export function isFinished(item) {
  return FINISHED.has(item.state);
}

export function isBusy(item) {
  return item.state === State.QUEUED || item.state === State.CONVERTING;
}

/** Creates a queue item for a file-like object ({name, size}). */
export function createItem(key, file) {
  return {
    key,
    file,
    name: file.name || 'Untitled',
    size: file.size ?? 0,
    state: State.DETECTING,
    source: null,
    sourceDetected: false,
    category: null,
    target: null,
    options: {},
    progress: 0,
    stage: '',
    result: null,
    error: null,
  };
}

/**
 * Applies an event and returns the next item (a new object), or the same
 * object when the event is not allowed in the current state.
 */
export function reduce(item, event) {
  if (!canApply(item, event.type)) return item;
  const next = { ...item, state: TRANSITIONS[item.state][event.type] };
  switch (event.type) {
    case 'detected':
      next.source = event.format;
      next.sourceDetected = Boolean(event.detected ?? true);
      next.category = event.category ?? null;
      next.target = event.target ?? null;
      break;
    case 'setTarget':
      next.target = event.target;
      if (event.target !== item.target) next.options = {};
      next.result = null;
      next.error = null;
      next.progress = 0;
      next.stage = '';
      break;
    case 'setOptions':
      next.options = { ...item.options, ...event.options };
      for (const [k, v] of Object.entries(next.options)) if (v === undefined) delete next.options[k];
      if (isFinished(item)) {
        next.result = null;
        next.error = null;
        next.progress = 0;
        next.stage = '';
      }
      break;
    case 'enqueue':
      next.progress = 0;
      next.stage = 'Waiting';
      next.result = null;
      next.error = null;
      break;
    case 'start':
      next.progress = 0;
      next.stage = 'Starting';
      break;
    case 'progress':
      next.progress = Math.max(item.progress, Math.min(1, Number(event.fraction) || 0));
      next.stage = event.stage || item.stage;
      break;
    case 'succeed':
      next.progress = 1;
      next.stage = 'Done';
      next.result = event.result;
      break;
    case 'fail':
      next.error = event.error;
      next.stage = '';
      break;
    case 'cancel':
      next.stage = '';
      next.progress = 0;
      break;
    case 'reset':
      next.progress = 0;
      next.stage = '';
      next.result = null;
      next.error = null;
      break;
    default:
      break;
  }
  return next;
}

/** An ordered, observable collection of items. Recent conversions live in memory only. */
export class Queue {
  constructor() {
    this.items = [];
    this.listeners = new Set();
    this.counter = 0;
  }

  subscribe(fn) {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  emit(change) {
    for (const fn of this.listeners) fn(change);
  }

  get(key) {
    return this.items.find((i) => i.key === key) || null;
  }

  add(files) {
    const added = [];
    for (const file of files) {
      this.counter += 1;
      const item = createItem(`f${this.counter}`, file);
      this.items.push(item);
      added.push(item);
    }
    if (added.length) this.emit({ type: 'add', items: added });
    return added;
  }

  /** Applies an event to one item. Returns true when the state machine accepted it. */
  dispatch(key, event) {
    const index = this.items.findIndex((i) => i.key === key);
    if (index < 0) return false;
    const before = this.items[index];
    const after = reduce(before, event);
    if (after === before) return false;
    this.items[index] = after;
    this.emit({ type: 'update', item: after, previous: before, event });
    return true;
  }

  remove(key) {
    const index = this.items.findIndex((i) => i.key === key);
    if (index < 0) return null;
    const [item] = this.items.splice(index, 1);
    this.emit({ type: 'remove', item });
    return item;
  }

  clear(predicate = () => true) {
    const removed = this.items.filter(predicate);
    this.items = this.items.filter((i) => !predicate(i));
    for (const item of removed) this.emit({ type: 'remove', item });
    return removed;
  }

  /** Source formats shared by two or more files: [{source, keys, count}] in first-seen order. */
  sourceGroups() {
    const groups = new Map();
    for (const item of this.items) {
      if (!item.source || item.state === State.DETECTING) continue;
      if (!groups.has(item.source)) groups.set(item.source, []);
      groups.get(item.source).push(item.key);
    }
    return [...groups.entries()].filter(([, keys]) => keys.length > 1).map(([source, keys]) => ({ source, keys, count: keys.length }));
  }

  /** Sets the target of every non-busy file of one source format. Returns the number changed. */
  setTargetForSource(source, target) {
    let changed = 0;
    for (const item of [...this.items]) {
      if (item.source === source && item.target !== target && !isBusy(item) && this.dispatch(item.key, { type: 'setTarget', target })) changed += 1;
    }
    return changed;
  }

  /** Items that "Convert all" would start. */
  convertible() {
    return this.items.filter((i) => canApply(i, 'enqueue') && (i.state === State.READY || i.state === State.CANCELLED || i.state === State.FAILED));
  }

  summary() {
    const counts = Object.fromEntries(Object.values(State).map((s) => [s, 0]));
    for (const item of this.items) counts[item.state] += 1;
    const busy = counts[State.QUEUED] + counts[State.CONVERTING];
    const running = this.items.filter(isBusy);
    const overall = running.length ? running.reduce((a, i) => a + i.progress, 0) / running.length : 0;
    return { total: this.items.length, counts, busy, done: counts[State.DONE], overall };
  }
}

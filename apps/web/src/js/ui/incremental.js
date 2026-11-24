// Convertible web app - incremental list rendering for long lists (the
// catalogue has well over a thousand formats). The first rows are built
// synchronously so the list appears at once; the rest follow in small
// batches between frames, so typing and scrolling never wait for the whole
// list. A newer render (or cancel()) abandons the batches still pending.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

/** Runs `fn` on the next frame, or after a short timeout where frames are throttled. */
function nextTick(fn) {
  let done = false;
  const run = () => {
    if (done) return;
    done = true;
    fn();
  };
  if (typeof requestAnimationFrame === 'function') requestAnimationFrame(run);
  setTimeout(run, 32);
}

/**
 * Creates a renderer bound to `root` (which gets aria-busy while batches are
 * pending). `render(jobs)` takes [{parent, items, make}] and appends
 * make(item) for every item to its parent, in order.
 */
export function createIncrementalRenderer(root, { first = 120, batch = 160 } = {}) {
  let generation = 0;
  let pending = [];
  let onDone = null;

  const step = (count) => {
    let left = count;
    while (pending.length && left > 0) {
      const job = pending[0];
      const end = Math.min(job.items.length, job.index + left);
      const frag = document.createDocumentFragment();
      for (let i = job.index; i < end; i += 1) frag.append(job.make(job.items[i], i));
      job.parent.append(frag);
      left -= end - job.index;
      job.index = end;
      if (job.index >= job.items.length) pending.shift();
    }
    if (!pending.length) {
      root.removeAttribute('aria-busy');
      const done = onDone;
      onDone = null;
      if (done) done();
    }
  };

  const schedule = (gen) => {
    nextTick(() => {
      if (gen !== generation || !pending.length) return;
      step(batch);
      if (pending.length) schedule(gen);
    });
  };

  return {
    /** Starts a new render; resolves `done` when every row exists. */
    render(jobs, done = null) {
      generation += 1;
      pending = jobs.filter((j) => j.items.length).map((j) => ({ ...j, index: 0 }));
      onDone = done;
      if (pending.length) root.setAttribute('aria-busy', 'true');
      step(first);
      if (pending.length) schedule(generation);
    },
    /** Builds every remaining row now (keyboard navigation, jumps). */
    flush() {
      if (pending.length) step(Number.POSITIVE_INFINITY);
    },
    get busy() {
      return pending.length > 0;
    },
    cancel() {
      generation += 1;
      pending = [];
      onDone = null;
      root.removeAttribute('aria-busy');
    },
  };
}

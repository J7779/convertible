// Convertible web app - main-thread client for the core workers.
//
// One "catalogue" worker answers the quick questions (formats, detection,
// targets, option schemas) so they are never stuck behind a long
// conversion; a small pool of conversion workers runs files in parallel.
//
// Cancelling a queued job removes it before it starts. Cancelling a running
// job terminates its worker and starts a fresh one (the only way to stop a
// synchronous WebAssembly call without SharedArrayBuffer), so cancel is
// immediate and never leaks memory.
//
// The worker factory is injected, which keeps this module testable in Node.
//
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { Status } from './binding.js';

export class CancelledError extends Error {
  constructor() {
    super('cancelled');
    this.name = 'CancelledError';
    this.status = Status.CANCELLED;
  }
}

export class WorkerError extends Error {
  constructor(status, message) {
    super(message);
    this.name = 'WorkerError';
    this.status = status;
  }
}

class Channel {
  constructor(createWorker) {
    this.createWorker = createWorker;
    this.pending = new Map();
    this.nextId = 1;
    this.spawn();
  }

  spawn() {
    const worker = this.createWorker();
    this.worker = worker;
    this.dead = false;
    worker.addEventListener('message', (event) => this.receive(event.data));
    worker.addEventListener('error', (event) => {
      if (event && typeof event.preventDefault === 'function') event.preventDefault();
      if (worker !== this.worker) return;
      // Replace the worker lazily (on the next request), so a worker that
      // cannot start at all does not respawn in a loop.
      this.dead = true;
      try {
        worker.terminate();
      } catch {
        /* already gone */
      }
      this.failAll(new WorkerError(Status.INTERNAL, 'the converter stopped unexpectedly'));
    });
  }

  restart() {
    try {
      this.worker.terminate();
    } catch {
      /* already gone */
    }
    this.spawn();
  }

  failAll(error) {
    const waiting = [...this.pending.values()];
    this.pending.clear();
    for (const p of waiting) p.reject(error);
  }

  send(message, { onProgress, transfer = [] } = {}) {
    const id = this.nextId++;
    if (this.dead) this.spawn();
    return {
      id,
      promise: new Promise((resolve, reject) => {
        this.pending.set(id, { resolve, reject, onProgress });
        this.worker.postMessage({ ...message, id }, transfer);
      }),
    };
  }

  receive(msg) {
    const entry = this.pending.get(msg.id);
    if (!entry) return;
    if (msg.type === 'progress') {
      if (entry.onProgress) entry.onProgress(msg.fraction, msg.stage);
      return;
    }
    this.pending.delete(msg.id);
    if (msg.type === 'error') entry.reject(new WorkerError(msg.status, msg.message));
    else entry.resolve(msg);
  }
}

export class CorePool {
  /**
   * @param {object} opts
   * @param {() => Worker} opts.createWorker
   * @param {number} [opts.size]  conversion workers (default from hardwareConcurrency, 1..3)
   */
  constructor({ createWorker, size } = {}) {
    this.createWorker = createWorker;
    const cores = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 2;
    this.size = Math.max(1, Math.min(3, size ?? Math.floor(cores / 2)));
    this.catalogue = new Channel(createWorker);
    this.slots = [];
    this.queue = []; // jobs waiting for a slot
    this.jobs = new Map(); // key -> job
  }

  // -- quick requests ----------------------------------------------------------

  request(type, payload = {}) {
    return this.catalogue.send({ type, ...payload }).promise;
  }

  async loadCatalogue() {
    const reply = await this.request('catalogue');
    return { version: reply.version, formats: reply.formats };
  }

  async detect(blob, name) {
    return (await this.request('detect', { blob, name })).format ?? null;
  }

  async targets(source) {
    return (await this.request('targets', { source })).targets ?? [];
  }

  async schema(source, target) {
    return (await this.request('schema', { source, target })).schema ?? [];
  }

  // -- conversions ---------------------------------------------------------------

  /**
   * Queues a conversion. Resolves with the worker's result message
   * ({ok, status, message, buffer, format, extension, mime, warnings});
   * rejects with CancelledError when cancelled and WorkerError on failure.
   */
  convert(key, { blob, name, from, to, options }, { onProgress, onStart } = {}) {
    if (this.jobs.has(key)) this.cancel(key);
    return new Promise((resolve, reject) => {
      const job = { key, message: { type: 'convert', blob, name, from, to, options }, onProgress, onStart, resolve, reject, slot: null };
      this.jobs.set(key, job);
      this.queue.push(job);
      this.pump();
    });
  }

  /** Number of jobs queued or running. */
  get activeCount() {
    return this.jobs.size;
  }

  isRunning(key) {
    const job = this.jobs.get(key);
    return Boolean(job && job.slot);
  }

  cancel(key) {
    const job = this.jobs.get(key);
    if (!job) return false;
    this.jobs.delete(key);
    if (job.slot) {
      const slot = job.slot;
      slot.job = null;
      slot.channel.pending.clear();
      slot.channel.restart(); // hard cancel
    } else {
      this.queue = this.queue.filter((j) => j !== job);
    }
    job.reject(new CancelledError());
    this.pump();
    return true;
  }

  cancelAll() {
    // Queued first, so a slot freed by a hard cancel does not pick them up.
    const keys = [...this.queue.map((j) => j.key), ...[...this.jobs.keys()]];
    for (const key of new Set(keys)) this.cancel(key);
  }

  pump() {
    while (this.queue.length) {
      let slot = this.slots.find((s) => !s.job);
      if (!slot && this.slots.length < this.size) {
        // A crashed worker rejects its job through failAll() and is respawned;
        // run()'s rejection handler frees the slot and keeps pumping.
        slot = { channel: new Channel(this.createWorker), job: null };
        this.slots.push(slot);
      }
      if (!slot) return;
      const job = this.queue.shift();
      this.run(slot, job);
    }
  }

  run(slot, job) {
    slot.job = job;
    job.slot = slot;
    if (job.onStart) job.onStart();
    const { promise } = slot.channel.send(job.message, { onProgress: job.onProgress });
    promise.then(
      (reply) => {
        if (slot.job !== job) return; // cancelled meanwhile
        slot.job = null;
        this.jobs.delete(job.key);
        job.resolve(reply);
        this.pump();
      },
      (error) => {
        if (slot.job !== job) return;
        slot.job = null;
        this.jobs.delete(job.key);
        job.reject(error);
        this.pump();
      },
    );
  }

  destroy() {
    this.cancelAll();
    this.catalogue.worker.terminate();
    for (const s of this.slots) s.channel.worker.terminate();
  }
}

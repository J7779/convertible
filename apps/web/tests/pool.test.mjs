// Worker pool tests: scheduling, soft cancel of queued jobs, hard cancel
// (terminate + respawn) of running jobs, crash recovery. Fake workers
// implement the worker message protocol and run conversions on the real core.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { before, test } from 'node:test';
import { importSrc, loadCore, text, utf8 } from './helpers.mjs';

const { CorePool, CancelledError, WorkerError } = await importSrc('js/core/pool.js');
let core;
before(async () => {
  core = await loadCore();
});

class FakeWorker {
  static all = [];
  constructor({ autoFinish = true } = {}) {
    this.listeners = { message: [], error: [] };
    this.terminated = false;
    this.autoFinish = autoFinish;
    this.inbox = [];
    FakeWorker.all.push(this);
  }
  addEventListener(type, fn) {
    this.listeners[type].push(fn);
  }
  emit(type, data) {
    if (this.terminated) return;
    for (const fn of this.listeners[type]) fn(type === 'message' ? { data } : data);
  }
  terminate() {
    this.terminated = true;
  }
  postMessage(msg) {
    this.inbox.push(msg);
    if (this.autoFinish) setTimeout(() => this.finish(msg), 1);
  }
  async finish(msg) {
    if (this.terminated) return;
    if (msg.type === 'catalogue') return this.emit('message', { type: 'reply', id: msg.id, version: core.version(), formats: core.formats() });
    if (msg.type === 'targets') return this.emit('message', { type: 'reply', id: msg.id, targets: core.targets(msg.source) });
    if (msg.type === 'convert') {
      const bytes = new Uint8Array(await msg.blob.arrayBuffer());
      const r = core.convert(bytes, {
        to: msg.to,
        from: msg.from,
        onProgress: (fraction, stage) => this.emit('message', { type: 'progress', id: msg.id, fraction, stage }),
      });
      return this.emit('message', { type: 'result', id: msg.id, ...r, data: undefined, buffer: r.data?.buffer ?? null });
    }
    return this.emit('message', { type: 'error', id: msg.id, status: 1, message: 'unknown' });
  }
}

const blob = (s) => new Blob([s]);

test('catalogue requests go to their own worker', async () => {
  FakeWorker.all = [];
  const pool = new CorePool({ createWorker: () => new FakeWorker(), size: 2 });
  const { formats, version } = await pool.loadCatalogue();
  assert.ok(formats.length > 0);
  assert.match(version, /^\d/);
  assert.ok((await pool.targets('txt')).some((t) => t.id === 'base64'));
  assert.equal(FakeWorker.all.length, 1, 'conversion workers start lazily');
});

test('conversions run in parallel up to the pool size and report progress', async () => {
  FakeWorker.all = [];
  const pool = new CorePool({ createWorker: () => new FakeWorker(), size: 2 });
  const progress = [];
  const started = [];
  const jobs = ['a', 'b', 'c'].map((k) =>
    pool.convert(k, { blob: blob(`file ${k}`), name: `${k}.txt`, from: 'txt', to: 'base64' }, { onProgress: (f) => progress.push([k, f]), onStart: () => started.push(k) }),
  );
  assert.equal(pool.activeCount, 3);
  assert.deepEqual(started, ['a', 'b'], 'third job waits for a free worker');
  const results = await Promise.all(jobs);
  assert.deepEqual(started, ['a', 'b', 'c']);
  assert.equal(text(new Uint8Array(results[2].buffer)).trim(), Buffer.from('file c').toString('base64'));
  assert.ok(progress.some(([k, f]) => k === 'a' && f === 1));
  assert.equal(FakeWorker.all.length, 3, '1 catalogue + 2 conversion workers');
  assert.equal(pool.activeCount, 0);
});

test('cancelling a queued job never starts it', async () => {
  FakeWorker.all = [];
  const pool = new CorePool({ createWorker: () => new FakeWorker({ autoFinish: false }), size: 1 });
  const first = pool.convert('a', { blob: blob('a'), from: 'txt', to: 'hex' });
  const second = pool.convert('b', { blob: blob('b'), from: 'txt', to: 'hex' });
  assert.equal(pool.cancel('b'), true);
  await assert.rejects(second, CancelledError);
  const worker = FakeWorker.all[1];
  assert.equal(worker.inbox.filter((m) => m.type === 'convert').length, 1);
  pool.cancel('a');
  await assert.rejects(first, CancelledError);
  assert.equal(pool.cancel('missing'), false);
});

test('cancelling a running job terminates its worker and a fresh one takes the next job', async () => {
  FakeWorker.all = [];
  const pool = new CorePool({ createWorker: () => new FakeWorker({ autoFinish: false }), size: 1 });
  const running = pool.convert('a', { blob: blob('a'), from: 'txt', to: 'hex' });
  const waiting = pool.convert('b', { blob: blob('b'), from: 'txt', to: 'hex' });
  const stuck = FakeWorker.all[1];
  assert.ok(pool.isRunning('a'));
  pool.cancel('a');
  await assert.rejects(running, CancelledError);
  assert.equal(stuck.terminated, true);
  const fresh = FakeWorker.all[2];
  assert.ok(fresh && !fresh.terminated);
  assert.equal(fresh.inbox[0].type, 'convert');
  fresh.finish(fresh.inbox[0]);
  const r = await waiting;
  assert.equal(r.ok, true);
  assert.equal(text(new Uint8Array(r.buffer)).replace(/\s/g, ''), '62');
});

test('cancelAll stops queued and running jobs', async () => {
  FakeWorker.all = [];
  const pool = new CorePool({ createWorker: () => new FakeWorker({ autoFinish: false }), size: 2 });
  const jobs = ['a', 'b', 'c', 'd'].map((k) => pool.convert(k, { blob: blob(k), from: 'txt', to: 'hex' }));
  pool.cancelAll();
  for (const j of jobs) await assert.rejects(j, CancelledError);
  assert.equal(pool.activeCount, 0);
  const convertsAfter = FakeWorker.all.filter((w) => !w.terminated).flatMap((w) => w.inbox).filter((m) => m.type === 'convert');
  assert.equal(convertsAfter.length, 0, 'no queued job slipped into a fresh worker');
});

test('a crashed worker fails its job and is replaced', async () => {
  FakeWorker.all = [];
  const pool = new CorePool({ createWorker: () => new FakeWorker({ autoFinish: false }), size: 1 });
  const job = pool.convert('a', { blob: blob('a'), from: 'txt', to: 'hex' });
  FakeWorker.all[1].emit('error', { preventDefault() {} });
  await assert.rejects(job, WorkerError);
  const next = pool.convert('b', { blob: blob('b'), from: 'txt', to: 'hex' });
  const replacement = FakeWorker.all.at(-1);
  assert.notEqual(replacement, FakeWorker.all[1]);
  replacement.finish(replacement.inbox.at(-1));
  assert.equal((await next).ok, true);
});

test('core errors come back as results with a status', async () => {
  FakeWorker.all = [];
  const pool = new CorePool({ createWorker: () => new FakeWorker(), size: 1 });
  const r = await pool.convert('x', { blob: new Blob([utf8('@@!!')]), from: 'base64', to: 'bin' });
  assert.equal(r.ok, false);
  assert.equal(r.status, 4);
  await assert.rejects(pool.request('bogus'), WorkerError);
});

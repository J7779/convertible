// Queue state machine tests.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { importSrc } from './helpers.mjs';

const { Queue, State, createItem, reduce, canApply, isBusy, isFinished } = await importSrc('js/model/queue.js');

const file = (name, size = 10) => ({ name, size });

test('happy path: detecting -> ready -> queued -> converting -> done', () => {
  let item = createItem('a', file('a.txt'));
  assert.equal(item.state, State.DETECTING);
  item = reduce(item, { type: 'detected', format: 'txt', category: 'document', target: 'base64' });
  assert.equal(item.state, State.READY);
  assert.equal(item.target, 'base64');
  item = reduce(item, { type: 'enqueue' });
  assert.equal(item.state, State.QUEUED);
  assert.ok(isBusy(item));
  item = reduce(item, { type: 'start' });
  item = reduce(item, { type: 'progress', fraction: 0.4, stage: 'Reading' });
  assert.equal(item.progress, 0.4);
  item = reduce(item, { type: 'progress', fraction: 0.2, stage: 'Writing' });
  assert.equal(item.progress, 0.4, 'progress never goes backwards');
  assert.equal(item.stage, 'Writing');
  item = reduce(item, { type: 'succeed', result: { size: 4 } });
  assert.equal(item.state, State.DONE);
  assert.equal(item.progress, 1);
  assert.ok(isFinished(item));
});

test('invalid events leave the item untouched', () => {
  const item = createItem('a', file('a.txt'));
  assert.equal(reduce(item, { type: 'enqueue' }), item);
  assert.equal(reduce(item, { type: 'succeed' }), item);
  const ready = reduce(item, { type: 'detected', format: 'bin', target: null });
  assert.equal(canApply(ready, 'enqueue'), false, 'no target, no conversion');
  assert.equal(reduce(ready, { type: 'start' }), ready);
});

test('cancel, fail, reset and retry', () => {
  let item = reduce(createItem('a', file('a')), { type: 'detected', format: 'txt', target: 'hex' });
  item = reduce(reduce(item, { type: 'enqueue' }), { type: 'cancel' });
  assert.equal(item.state, State.CANCELLED);
  item = reduce(item, { type: 'enqueue' });
  item = reduce(reduce(item, { type: 'start' }), { type: 'fail', error: { status: 4, title: 'x' } });
  assert.equal(item.state, State.FAILED);
  assert.equal(item.error.status, 4);
  item = reduce(item, { type: 'enqueue' });
  assert.equal(item.state, State.QUEUED);
  assert.equal(item.error, null);
  item = reduce(reduce(reduce(item, { type: 'start' }), { type: 'succeed', result: { size: 1 } }), { type: 'reset' });
  assert.equal(item.state, State.READY);
  assert.equal(item.result, null);
});

test('permanent errors (file too large) cannot be retried', () => {
  let item = createItem('a', file('huge', 5e9));
  item = reduce(item, { type: 'fail', error: { status: 8, permanent: true } });
  assert.equal(item.state, State.FAILED);
  assert.equal(canApply({ ...item, target: 'hex' }, 'enqueue'), false);
});

test('changing the target or options of a finished file returns it to ready', () => {
  let item = reduce(createItem('a', file('a')), { type: 'detected', format: 'txt', target: 'hex' });
  item = reduce(item, { type: 'setOptions', options: { line_width: 0 } });
  assert.deepEqual(item.options, { line_width: 0 });
  item = reduce(item, { type: 'setTarget', target: 'base64' });
  assert.deepEqual(item.options, {}, 'options belong to a target');
  item = reduce(reduce(reduce(item, { type: 'enqueue' }), { type: 'start' }), { type: 'succeed', result: { size: 1 } });
  item = reduce(item, { type: 'setOptions', options: { preset: 'small' } });
  assert.equal(item.state, State.READY);
  assert.equal(item.result, null);
  item = reduce(item, { type: 'setOptions', options: { preset: undefined } });
  assert.deepEqual(item.options, {});
});

test('busy items ignore target changes', () => {
  let item = reduce(createItem('a', file('a')), { type: 'detected', format: 'txt', target: 'hex' });
  item = reduce(item, { type: 'enqueue' });
  assert.equal(reduce(item, { type: 'setTarget', target: 'base64' }), item);
});

test('Queue: add, dispatch, events, groups, set-all, summary, clear', () => {
  const q = new Queue();
  const events = [];
  q.subscribe((e) => events.push(e.type));
  const [a, b, c, d] = q.add([file('a.txt'), file('b.txt'), file('c.png'), file('d.txt')]);
  assert.equal(q.items.length, 4);
  assert.notEqual(a.key, b.key);
  assert.deepEqual(events, ['add']);

  for (const [item, fmt] of [[a, 'txt'], [b, 'txt'], [c, 'png'], [d, 'txt']]) {
    assert.ok(q.dispatch(item.key, { type: 'detected', format: fmt, target: fmt === 'txt' ? 'base64' : 'hex' }));
  }
  assert.equal(q.dispatch(a.key, { type: 'succeed' }), false);
  assert.deepEqual(q.sourceGroups(), [{ source: 'txt', keys: [a.key, b.key, d.key], count: 3 }]);

  q.dispatch(d.key, { type: 'enqueue' });
  assert.equal(q.setTargetForSource('txt', 'hex'), 2, 'busy files keep their target');
  assert.equal(q.get(a.key).target, 'hex');
  assert.equal(q.get(d.key).target, 'base64');

  assert.deepEqual(q.convertible().map((i) => i.key), [a.key, b.key, c.key]);
  q.dispatch(d.key, { type: 'start' });
  q.dispatch(d.key, { type: 'progress', fraction: 0.5 });
  const s = q.summary();
  assert.equal(s.total, 4);
  assert.equal(s.busy, 1);
  assert.equal(s.overall, 0.5);

  const removed = q.clear((i) => !isBusy(i));
  assert.equal(removed.length, 3);
  assert.deepEqual(q.items.map((i) => i.key), [d.key]);
  assert.equal(q.remove('nope'), null);
  assert.equal(q.remove(d.key).key, d.key);
  assert.equal(q.items.length, 0);
});

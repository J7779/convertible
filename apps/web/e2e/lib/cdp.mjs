// End-to-end tests - a minimal Chrome DevTools Protocol client over the
// built-in WebSocket (Node 22+). One connection to the browser; targets are
// reached through flattened sessions (`sessionId` on every message).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

/** E2E_DEBUG=1 logs every command and its reply to stderr. */
const DEBUG = process.env.E2E_DEBUG === '1';

export class ProtocolError extends Error {
  constructor(method, error) {
    super(`${method}: ${error.message}${error.data ? ` (${error.data})` : ''}`);
    this.name = 'ProtocolError';
    this.code = error.code;
  }
}

export class Connection {
  /** @param {WebSocket} ws */
  constructor(ws) {
    this.ws = ws;
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = new Set();
    this.closed = false;
    ws.addEventListener('message', (event) => this.receive(JSON.parse(typeof event.data === 'string' ? event.data : Buffer.from(event.data).toString('utf8'))));
    ws.addEventListener('close', () => {
      this.closed = true;
      const error = new Error('the browser connection closed');
      for (const p of this.pending.values()) p.reject(error);
      this.pending.clear();
    });
  }

  static async connect(url) {
    const ws = new WebSocket(url);
    await new Promise((resolve, reject) => {
      ws.addEventListener('open', resolve, { once: true });
      ws.addEventListener('error', () => reject(new Error(`cannot connect to ${url}`)), { once: true });
    });
    return new Connection(ws);
  }

  /** Sends a command; resolves with its result or rejects with a ProtocolError. */
  send(method, params = {}, sessionId = undefined) {
    if (this.closed) return Promise.reject(new Error(`${method}: the browser connection is closed`));
    const id = this.nextId++;
    const message = { id, method, params };
    if (sessionId) message.sessionId = sessionId;
    if (DEBUG) console.error(`cdp > ${id} ${method} ${sessionId ? sessionId.slice(0, 6) : 'browser'}`);
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, method });
      this.ws.send(JSON.stringify(message));
    });
  }

  receive(message) {
    if (message.id !== undefined) {
      const entry = this.pending.get(message.id);
      if (!entry) return;
      this.pending.delete(message.id);
      if (DEBUG) console.error(`cdp < ${message.id} ${entry.method}${message.error ? ` error ${message.error.message}` : ''}`);
      if (message.error) entry.reject(new ProtocolError(entry.method, message.error));
      else entry.resolve(message.result);
      return;
    }
    for (const fn of [...this.listeners]) {
      try {
        fn(message);
      } catch (error) {
        // A listener bug must not take the connection down; surface it.
        console.error('CDP listener failed:', error);
      }
    }
  }

  /** Subscribes to every event: fn({method, params, sessionId}). Returns an unsubscribe function. */
  onEvent(fn) {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  /**
   * Resolves with the params of the first event matching `method` (and
   * `sessionId` and `predicate`, when given); rejects after `timeout` ms.
   */
  waitForEvent(method, { sessionId = undefined, predicate = () => true, timeout = 10_000 } = {}) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        off();
        reject(new Error(`timed out after ${timeout} ms waiting for ${method}`));
      }, timeout);
      const off = this.onEvent((m) => {
        if (m.method !== method) return;
        if (sessionId !== undefined && m.sessionId !== sessionId) return;
        if (!predicate(m.params)) return;
        clearTimeout(timer);
        off();
        resolve(m.params);
      });
    });
  }

  close() {
    try {
      this.ws.close();
    } catch {
      /* already closed */
    }
  }
}

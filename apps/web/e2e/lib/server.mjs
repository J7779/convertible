// End-to-end tests - runs the app's own zero-dependency server
// (tools/serve.mjs) on a free port against a dist directory.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import path from 'node:path';
import { WEB_DIR } from './paths.mjs';

function freePort() {
  return new Promise((resolve, reject) => {
    const srv = createServer();
    srv.unref();
    srv.on('error', reject);
    srv.listen(0, '127.0.0.1', () => {
      const { port } = srv.address();
      srv.close(() => resolve(port));
    });
  });
}

async function startOnce(dir, port) {
  const child = spawn(process.execPath, [path.join(WEB_DIR, 'tools', 'serve.mjs'), '--port', String(port), '--host', '127.0.0.1', '--dir', dir], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`the server did not start:\n${output}`)), 15_000);
    const onData = (chunk) => {
      output += chunk;
      if (output.includes('Serving ')) {
        clearTimeout(timer);
        resolve();
      }
    };
    child.stdout.setEncoding('utf8').on('data', onData);
    child.stderr.setEncoding('utf8').on('data', onData);
    child.once('exit', (code) => {
      clearTimeout(timer);
      reject(new Error(`the server exited with ${code}:\n${output}`));
    });
  });
  child.stdout.resume();
  child.stderr.resume();
  return child;
}

/**
 * Starts tools/serve.mjs for `dir`. Resolves with {url, port, close()}; the
 * url ends with a slash. Retries on the rare port race.
 */
export async function startServer(dir) {
  let lastError = null;
  for (let attempt = 0; attempt < 5; attempt += 1) {
    const port = await freePort();
    try {
      const child = await startOnce(dir, port);
      return {
        port,
        url: `http://127.0.0.1:${port}/`,
        origin: `http://127.0.0.1:${port}`,
        close: () =>
          new Promise((resolve) => {
            if (child.exitCode !== null || child.signalCode !== null) return resolve();
            child.once('exit', () => resolve());
            child.kill();
          }),
      };
    } catch (error) {
      lastError = error;
    }
  }
  throw lastError;
}

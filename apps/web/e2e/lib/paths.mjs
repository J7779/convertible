// End-to-end tests - locations shared by the helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const E2E_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const WEB_DIR = path.resolve(E2E_DIR, '..');
export const DIST = path.resolve(process.env.CVT_WEB_DIST || path.join(WEB_DIR, 'dist'));
/** Failure screenshots and logs (git-ignored). */
export const ARTIFACTS = path.join(E2E_DIR, 'artifacts');

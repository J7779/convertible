// Convertible web app - installable PWA plumbing: service worker with an
// update prompt, install button, files opened through the OS (file
// handlers) and files shared to the app (share target).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.

/**
 * Registers the service worker. `onUpdate(apply)` is called when a new
 * version has been downloaded and is waiting; calling `apply()` activates it
 * and reloads the page.
 */
export async function registerServiceWorker({ onUpdate, onOfflineReady } = {}) {
  if (!('serviceWorker' in navigator)) return null;
  let reloading = false;
  let requested = false;
  navigator.serviceWorker.addEventListener('controllerchange', () => {
    if (!requested || reloading) return;
    reloading = true;
    window.location.reload();
  });
  const hadController = Boolean(navigator.serviceWorker.controller);
  let registration;
  try {
    registration = await navigator.serviceWorker.register('./sw.js', { scope: './', updateViaCache: 'none' });
  } catch {
    return null;
  }
  const offer = (worker) => {
    if (!worker || !onUpdate) return;
    onUpdate(() => {
      requested = true;
      worker.postMessage({ type: 'skip-waiting' });
    });
  };
  if (registration.waiting && navigator.serviceWorker.controller) offer(registration.waiting);
  registration.addEventListener('updatefound', () => {
    const worker = registration.installing;
    if (!worker) return;
    worker.addEventListener('statechange', () => {
      if (worker.state !== 'installed') return;
      if (navigator.serviceWorker.controller) offer(worker);
      else if (!hadController && onOfflineReady) onOfflineReady();
    });
  });
  // Look for a new version whenever the app comes back to the foreground.
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') registration.update().catch(() => {});
  });
  return registration;
}

/** Shows `button` when the browser offers installation; clicking installs. */
export function setupInstall(button) {
  let deferred = null;
  window.addEventListener('beforeinstallprompt', (event) => {
    event.preventDefault();
    deferred = event;
    button.hidden = false;
  });
  window.addEventListener('appinstalled', () => {
    deferred = null;
    button.hidden = true;
  });
  button.addEventListener('click', async () => {
    if (!deferred) return;
    button.hidden = true;
    deferred.prompt();
    await deferred.userChoice.catch(() => null);
    deferred = null;
  });
}

export function isStandalone() {
  return matchMedia('(display-mode: standalone)').matches || navigator.standalone === true;
}

/** Files the OS opened with the installed app (manifest file_handlers). */
export function consumeLaunchFiles(addFiles) {
  if (!('launchQueue' in window)) return;
  window.launchQueue.setConsumer(async (params) => {
    if (!params.files || !params.files.length) return;
    const files = [];
    for (const handle of params.files) {
      try {
        files.push(await handle.getFile());
      } catch {
        /* the handle went away */
      }
    }
    if (files.length) addFiles(files);
  });
}

/** Files shared to the installed app (manifest share_target, stashed by sw.js). */
export async function consumeSharedFiles(addFiles) {
  const url = new URL(window.location.href);
  if (url.searchParams.get('share') !== '1') return;
  url.searchParams.delete('share');
  history.replaceState(null, '', url.pathname + (url.search || '') + url.hash);
  if (!('caches' in window)) return;
  const inbox = await caches.open('convertible-share-inbox');
  const files = [];
  for (const request of await inbox.keys()) {
    const response = await inbox.match(request);
    await inbox.delete(request);
    if (!response) continue;
    const blob = await response.blob();
    const name = decodeURIComponent(response.headers.get('X-File-Name') || 'Shared file');
    files.push(new File([blob], name, { type: blob.type }));
  }
  await caches.delete('convertible-share-inbox');
  if (files.length) addFiles(files);
}

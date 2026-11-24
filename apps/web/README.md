# Convertible for the web

The Convertible core compiled to WebAssembly, wrapped in an installable, fully
offline Progressive Web App. Files never leave the device: every conversion runs
in a Web Worker inside the browser, and after the first visit the app needs no
network at all.

No frameworks, no npm packages, no bundler, no CDN: hand-written HTML, CSS and
ES modules, plus the core as `convertible.wasm`.

## Prerequisites

- **To build:** Docker (the build runs in the pinned `emscripten/emsdk:7.0.0`
  image), *or* a locally activated emsdk 7.0.0 with CMake 3.22+. Nothing is
  installed on the host.
- **To test and serve:** Node.js 18 or newer (no packages).

## Build

```sh
apps/web/build.sh            # local emsdk if `emcmake` is on PATH, otherwise Docker
apps/web/build.sh --docker   # always the container (what CI uses)
apps/web/build.sh --local    # never the container
```

The output is a complete static site in `apps/web/dist/` (about 3 MB, of which
`wasm/convertible.wasm` is about 0.9 MB). Intermediate files go to
`apps/web/build/` (`build/wasm` for CMake, `build/emcache` for Emscripten's
system-library cache, which CI may cache between runs). Both directories are
ignored by git.

Environment knobs: `CVT_EMSDK_IMAGE` (default `emscripten/emsdk:7.0.0`),
`CVT_JOBS` (default 6), `CVT_DOCKER_MEMORY` (default `4g`).

What the build does:

1. `emcmake cmake -S apps/web -B apps/web/build/wasm` and `cmake --build` —
   `apps/web/CMakeLists.txt` adds `../../core` with `add_subdirectory`, links
   `convertible::core` into `convertible.mjs` + `convertible.wasm`.
2. `node apps/web/tools/assemble.mjs` — copies `src/`, the module, and the
   shared brand assets (`assets/brand/tokens.css`, fonts, icons, licences; they
   are never duplicated in git), turns `animation.json` into an ES module,
   generates `manifest.webmanifest` (its `file_handlers` list every format the
   real core can read) and `sw.js` with a precache list of every file and a
   content-digest cache version.

## Run

```sh
node apps/web/tools/serve.mjs            # http://127.0.0.1:8080/
node apps/web/tools/serve.mjs --port 9000 --host 0.0.0.0
```

The server sends the same security headers a production host should
(`Content-Security-Policy`, `nosniff`, `no-referrer`, `frame-ancestors 'none'`)
and `application/wasm` for the core. Any static host works: no
cross-origin isolation (COOP/COEP) is needed because the module uses no
threads and no `SharedArrayBuffer`. Service workers require HTTPS or
`localhost`.

## Test

```sh
apps/web/test.sh             # needs a build in apps/web/dist
apps/web/test.sh --build     # build first
```

58 tests on `node --test`:

- `binding.test.mjs` — loads the real `convertible.wasm` in Node and performs
  real conversions: text ↔ Base64, every byte value through hex and back,
  checksums against the standard vectors and Node's crypto, UTF-16LE/BOM and
  Windows-1252 re-encoding, every status code, progress order, cancellation
  from the callback, re-entrancy, and 1000 conversions with no heap growth.
- `pool.test.mjs` — worker scheduling, cancelling queued jobs, hard cancel by
  terminating and respawning a worker, cancel-all, crash recovery.
- `queue.test.mjs`, `options.test.mjs`, `targets.test.mjs`, `format.test.mjs` —
  the queue state machine, schema → controls model (also over the real
  catalogue), target grouping/sorting/search, size formatting, naming, error
  mapping, limits, settings.
- `dist.test.mjs` — every file in `dist` is precached and nothing else, the
  cache version is a content digest, all imports and assets resolve, no
  external URLs, a strict CSP without inline code, a valid installable
  manifest, and the motion storyboard → Web Animations conversion.

Tests are written against the dynamic catalogue, so they keep passing as the
core gains formats.

## Architecture

```
src/
  index.html, css/app.css         semantic HTML, tokens.css custom properties, container queries
  sw.js                           precache + cache-first + share target (template; list injected)
  manifest.template.json          name, icons, shortcuts, share_target (file_handlers added at build)
  js/core/binding.js              typed binding over the C ABI (strings, freeing, heap buffers, statuses)
  js/core/worker.js               dedicated worker that owns one WebAssembly instance
  js/core/pool.js                 main-thread client: catalogue worker + conversion pool, cancel
  js/model/*.js                   pure, DOM-free state: queue, options, targets, format, limits, settings
  js/ui/*.js                      views: cards, picker, sheets, intro/spinner/progress (motion)
  js/platform/*.js                file I/O (drop, folders, paste, save, share) and PWA plumbing
  js/app.js                       controller wiring the above together
tools/assemble.mjs, precache.mjs, serve.mjs
tests/*.test.mjs
```

**Binding → model → views.** Only `js/core` touches the C ABI; `js/model` is
plain logic tested in Node; `js/ui` renders it.

**Threads.** The main thread never runs core code. One worker answers catalogue
questions (formats, detection, targets, option schemas) and up to three
conversion workers (half the logical cores) convert in parallel, each with
its own instance. Files are posted as `Blob`s and streamed straight into the
WebAssembly heap (no second full copy in JavaScript); results come back as
transferred `ArrayBuffer`s.

**Cancellation.** The core stops when its progress callback returns non-zero,
but a busy worker cannot receive a message and `SharedArrayBuffer` is not
available without cross-origin isolation. So: cancelling a queued file
removes it before it reaches a worker (cooperative), and cancelling a running
file terminates that worker and starts a fresh one (immediate, and all of its
memory is released). The binding's `onProgress` returning `true` is the
in-process cancel path (used by the tests and by any future isolated build).

**Large files.** `cvt_convert_file` is exported but has no file system to map
here (`FILESYSTEM=0`); the web app converts in memory and refuses inputs
above 1 GB with a friendly message, because input, decoded content and output
must fit in a 32-bit heap together.

**Saving.** One file: the File System Access save dialog where available,
otherwise a download. Save all: a folder picker where available, otherwise
individual downloads. Share (Web Share with files) and Open (new tab) appear
when the browser supports them for that type.

**Offline and updates.** `sw.js` precaches everything into
`convertible-app-<digest>` and serves cache-first. A new deployment installs
in the background; the page shows "A new version is available — Reload",
which activates it and reloads. Old caches are deleted on activation.

**Installed app.** `file_handlers` + `launchQueue` open files from the OS;
`share_target` receives shared files (the service worker holds them in a
transient cache until the page takes them, then deletes them); shortcuts
open the picker, the formats browser or settings. iOS: apple-touch-icon,
standalone meta tags, safe-area insets.

**Privacy.** No network requests other than the app's own files, no
analytics, no history: results live in memory only. `localStorage` holds
exactly three preferences (theme, default quality, strip metadata).

## How the core is linked

`apps/web/CMakeLists.txt` configures only under Emscripten and builds the core
from source with the same CMake as every other app. Link settings: exactly the
`cvt_*` functions plus `malloc`/`free` exported, `-fwasm-exceptions` (from the
core target), `MODULARIZE` + `EXPORT_ES6`, `ALLOW_MEMORY_GROWTH` up to 4 GB,
`ALLOW_TABLE_GROWTH` (for the progress callback pointer), `FILESYSTEM=0`,
`DYNAMIC_EXECUTION=0` (CSP-friendly), `ENVIRONMENT=web,worker,node`, `-O3`,
no debug info, stripped. No pthreads.

## Hosting checklist

- Serve `dist/` as static files; `.wasm` as `application/wasm`.
- Send `Content-Security-Policy` as in `tools/serve.mjs` (the page also
  carries it in a `<meta>` tag; `frame-ancestors` only works as a header).
- Serve `sw.js` with `Cache-Control: no-cache` so updates are found promptly.

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

The output is a complete static site in `apps/web/dist/` (about 38 MB, of which
`wasm/convertible.wasm` is 35.9 MB, 13.2 MB with gzip -9, for the 1,485-format
catalogue). Intermediate files go to
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
   are never duplicated in git, and `NOTICES.md` for the components compiled
   into the core), turns `animation.json` into an ES module, asks the real core
   for its catalogue and writes it as `catalogue.json`, generates
   `manifest.webmanifest` (see [File handlers](#file-handlers)) and `sw.js` with
   a precache list of every file, each file's content hash, and a
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

62 tests on `node --test`:

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
- `loader.test.mjs` — the real core streamed in pieces with monotonic byte
  progress and compiled once; an unknown total for compressed responses;
  network, HTTP and compile failures classified for the start-up screen.
- `dist.test.mjs` — every file in `dist` is precached (with its content hash)
  and nothing else, the cache version is a content digest, all imports and
  assets resolve, no external URLs, a strict CSP without inline code, a valid
  installable manifest within the browser's file-handler limits, the shipped
  catalogue, and the motion storyboard → Web Animations conversion.

Tests are written against the dynamic catalogue, so they keep passing as the
core gains formats.

## End-to-end tests

```sh
apps/web/e2e.sh                                  # builds dist first if it is missing
apps/web/e2e.sh --test-name-pattern=offline      # any node --test option
```

The real app in a real headless Chrome or Chromium, performing real
conversions whose downloaded bytes are checked in Node (`Buffer`,
`node:crypto`, `TextDecoder`, an independent ZIP reader). No packages: an
in-house driver (`e2e/lib/`) speaks the Chrome DevTools Protocol over Node's
built-in `WebSocket`, and the app is served by `tools/serve.mjs` on a free
port.

Needs Node.js 22+ and Chrome/Chromium: `CHROME_BIN`, otherwise the usual
install locations on macOS, Linux and Windows. Without a browser the suite is
skipped with a note; `E2E_REQUIRE_BROWSER=1` makes that a failure (use it in
CI). With `CI` set, Chrome runs with `--no-sandbox` (as GitHub's Ubuntu
runners need). Other knobs: `E2E_CONCURRENCY` (test files in parallel,
default 2), `E2E_LONG_MB` (size of the cancel-test inputs, default 192),
`E2E_DEBUG=1` (log every protocol command).

- `e2e/lib/cdp.mjs`, `browser.mjs` — protocol client; Chrome launched with a
  temporary profile; one isolated browser context per test; pages, their
  workers and service workers are instrumented from their first request.
  Primitives: navigate, condition waits with timeouts, click/type/press,
  native file chooser, file inputs, drag-and-drop of files, downloads to a
  temporary directory, viewport/touch/colour-scheme/reduced-motion
  emulation, offline, screenshots.
- `e2e/lib/harness.mjs` — after every test: no console errors, no uncaught
  exceptions, no CSP violations, no failed requests and every request
  same-origin. A failing test leaves a screenshot and a log in
  `e2e/artifacts/` (git-ignored).
- 61 tests: `startup` (intro, skip, reduced motion, byte progress during a
  slow core download with the shell usable, a failed download and Retry),
  `queue` (picker, drag-and-drop, detected formats, bulk target, Convert all,
  Save all), `picker` (catalogue, groups, search, keyboard, Compress),
  `convert` (progress, sizes, names, bytes, schema-driven options, UTF-16,
  large files, native save dialog), `cancel`, `errors`, `sheets` (formats
  browser, settings persistence, licences, deep links), `keyboard`, `layout`
  (320/375/768/1440 px, dark and light), `pwa` (manifest, precache, offline
  reload and conversion, update prompt), `catalogue` (PNG → JPEG, any file →
  ZIP, WAV → audio equal to the core in Node), `media` (PNG → a
  browser-decodable image checked with `createImageBitmap`, WAV → compressed
  audio decoded with `decodeAudioData` for duration and level, a YUV4MPEG2
  clip → animated GIF with its frames counted, Markdown → PDF with `%PDF` and
  pages counted, CSV → XLSX → CSV and CSV → JSON cell by cell, a 3.6-gigapixel
  PNG refused with the size-limit message). Catalogue-dependent tests find
  their targets by category and MIME type and are skipped with "format not in
  catalogue" when the core cannot do them.

Downloads use the app's fallback for browsers without the File System Access
API (the tests remove it, since a headless browser cannot answer the native
save dialog); one test covers the native dialog path by answering it.
Waits are on conditions only, there are no retries, and every test starts
from a fresh browser context.

## Architecture

```
src/
  index.html, css/app.css         semantic HTML, tokens.css custom properties, container queries
  sw.js                           precache + cache-first + share target (template; list injected)
  manifest.template.json          name, icons, shortcuts, share_target (file_handlers added at build)
  js/core/binding.js              typed binding over the C ABI (strings, freeing, heap buffers, statuses)
  js/core/worker.js               dedicated worker that owns one WebAssembly instance
  js/core/pool.js                 main-thread client: catalogue worker + conversion pool, cancel
  js/core/loader.js               one streaming download + compile of the core, byte progress
  js/model/*.js                   pure, DOM-free state: queue, options, targets, format, limits, settings
  js/ui/*.js                      views: cards, picker, sheets, intro/spinner/progress (motion)
  js/platform/*.js                file I/O (drop, folders, paste, save, share) and PWA plumbing
  js/app.js                       controller wiring the above together
tools/assemble.mjs, precache.mjs, serve.mjs
tests/*.test.mjs
```

**Binding → model → views.** Only `js/core` touches the C ABI; `js/model` is
plain logic tested in Node; `js/ui` renders it.

**First load.** The page never waits for the core to show itself: the
catalogue ships as `catalogue.json` (counts, the formats browser and search
work at once), and the shell is usable while `js/core/loader.js` downloads
the core with `fetch` and compiles it while the bytes arrive
(`WebAssembly.compileStreaming` over a counting stream). The home screen shows
the brand progress mark filling with the bytes received ("12 MB of 35.9 MB";
the total is the uncompressed size recorded at build time, because
`Content-Length` is the compressed size when the host compresses), but only if
loading takes longer than 250 ms, so a cached start does not flash it. Files
added meanwhile wait for the core. A failed download says so (offline or not)
with a Retry button, and retries by itself when the browser comes back online.
The compiled `WebAssembly.Module` is posted to every worker, so the binary is
downloaded and compiled once, not once per worker.

**Threads.** The main thread never runs core code. One worker answers catalogue
questions (formats, detection, targets, option schemas) and up to three
conversion workers (half the logical cores) convert in parallel, each with
its own instance of the one compiled module. Files are posted as `Blob`s and streamed straight into the
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

**Long lists.** A source format can have over 400 targets and the catalogue
has about 1,500 formats. The picker and the formats browser build their first
rows at once and the rest in batches between frames (`js/ui/incremental.js`;
the list carries `aria-busy` until complete, and keyboard navigation past the
built rows builds the rest at once); off-screen browser groups skip layout and
paint (`content-visibility`). Search ranks exact id or extension, then
prefixes, then name words, then descriptions, and puts the group holding the
best match first. The picker offers category jump links when it has four or
more categories, and the browser filters by category.

**Offline and updates.** `sw.js` precaches everything into
`convertible-app-<digest>` and serves cache-first. It is registered only after
the page has loaded the core, so a first visit does not download the 36 MB
twice in parallel; the install then revalidates (`cache: 'no-cache'`), which a
host with validators answers with 304. Every file is checked against its
content hash from the build, so a deployment that lands mid-install cannot mix
versions, and a file whose hash is unchanged is copied from the previous
version's cache: an update that does not touch the core downloads only the
few changed files. The page shows "A new version is available — Reload",
which activates it and reloads. Old caches are deleted on activation.

### File handlers

Chromium accepts at most 300 file extensions across all of a manifest's
`file_handlers` and reports the rest as a manifest error, so the build takes
the most popular readable formats up to that limit (a format's extensions all
or none), one handler per category with the extensions grouped by MIME type
(`fileHandlers()` in `js/model/targets.js`). Every other readable format still
opens through the file chooser, drag and drop, paste and sharing.

### Keeping heavy codecs out of the first download (investigated)

The core offers no CMake option to leave families out (every
`src/families/*/family.cmake` is always included and `register_all.cpp`
registers every family), so the web build cannot make a smaller "base"
module without editing the core. Measured on this build (object sizes, a
proxy for code size): the imported codecs are roughly a quarter of the code
(libjxl 12.9 MB, libaom 6.8 MB, libvpx 3.8 MB, opencore-amr 2.6 MB, others
under 2 MB each) and the in-house families the rest (archive, document,
vector, office, geo and science 7–10 MB each). A media split would therefore
save roughly a third of the first download. What the core would need: a cache
variable such as `CVT_FAMILIES` (a list; default all) that includes only those
families' `family.cmake` and generates the registration list, so that
`apps/web/CMakeLists.txt` can link a base module (documents, data, archives,
images without JPEG XL/AVIF) and a media module (audio, video, codecs) from
the same sources. The app would load the media module on demand when a
source or target is missing from the base module's `cvt_targets_json`; the
catalogue the user sees would stay the union.

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
- Compress `.wasm` (gzip or Brotli: 35.9 MB becomes about 13 MB with gzip)
  and send validators (`ETag` or `Last-Modified`), so the service worker's
  install revalidates the core the page has just downloaded instead of
  fetching it again.

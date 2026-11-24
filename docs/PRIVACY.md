# Privacy

Convertible exists so that converting a file never means handing it to someone else. This
page states exactly what the apps do and do not do. It describes the code in this repository;
if you find behaviour that contradicts it, please report it as a security issue
([SECURITY.md](../SECURITY.md)).

## What Convertible does not do

- **No network.** The core contains no networking code, opens no sockets and starts no other
  programs. The apps make no network requests of their own: the Android app does not even
  request the `INTERNET` permission, and the web app only ever fetches its own files (its
  Content-Security-Policy forbids anything else; after the first visit it runs fully offline
  from its service-worker cache).
- **No accounts, no telemetry, no analytics, no crash reporting, no advertising.** Nothing
  about you, your files or your use of the app is collected or sent anywhere.
- **No history.** The apps do not keep a list of converted files. Results live in memory or in
  an app-private temporary folder until you save them, and are deleted when you remove them,
  when the app closes and again at the next start.
- **No reading of files you did not choose.** The core reads only the buffer or path it is
  given and writes only its result.

## What Convertible stores

Only your preferences, on your device:

| App | Stored | Where |
| --- | --- | --- |
| Web | theme, default quality, strip metadata | `localStorage` of the site's origin |
| iOS / iPadOS | the same three settings | `UserDefaults` |
| Android | the same three settings | app-private preferences |
| Desktop | settings as JSON | `~/Library/Application Support/Convertible` (macOS), `%APPDATA%\Convertible` (Windows), `$XDG_CONFIG_HOME/convertible` or `~/.config/convertible` (Linux) |
| CLI | nothing | |

## Metadata in your output

Files often carry more than their content: GPS position, camera serial numbers, author
names, software versions, edit history, embedded thumbnails. Every writer honours the
standard `strip_metadata` option, and it is **on by default**: unless you turn it off,
identifying metadata is left out of converted files. Writers never add identifying data of
their own: no "created by", no current date, no host or user name. Output is deterministic:
the same input and options give byte-identical output on every platform.

## Permissions

| App | Permissions |
| --- | --- |
| Android | none |
| iOS / iPadOS | "Add to Photos", asked only when you tap *Save to Photos*; the privacy manifest declares no tracking and no collected data |
| Web | none beyond what you grant per action (file and folder pickers, share sheet) |
| Desktop | none; ordinary file access through the system's dialogs and drag and drop |

## Hostile files

Files are untrusted input. Every reader is bounds-checked and checks size limits before
allocating; all of them are fuzzed and run under AddressSanitizer in CI; conversions run in
the app's own sandbox where the platform has one (web workers, the iOS and Android app
sandboxes).

## The repository itself

The source tree, test fixtures and commit history are kept free of personal and machine
data: no absolute paths, user or host names, e-mail addresses, keys or signing material.
`tools/privacy-audit --history` checks this and runs in CI on every push.

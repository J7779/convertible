# Contributing to Convertible

Thank you for helping. Convertible is a privacy-first, offline converter with an in-house
engine, so contributions follow a few firm rules. The full guide - project map, rules, exact
commands and the definition of done - is [AGENTS.md](AGENTS.md); the essentials are here.

## Before you start

- **Open an issue first** for anything larger than a bug fix: a new format, a new family, an
  imported codec or a UI change. Someone may already be working on it, and family ownership
  matters (see `core/README.md`, "Who owns what").
- **No new dependencies.** Write it in-house against the specification. Large codecs are the
  only exception and need the conditions in `core/README.md` ("Imported code").
- **No network code, telemetry or identifying metadata**, ever. See [docs/PRIVACY.md](docs/PRIVACY.md).

## Making a change

1. Build and test: `cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev`,
   then the `asan` preset for anything that parses input. App changes: that app's tests
   ([docs/TESTING.md](docs/TESTING.md)).
2. New formats come with real-file fixtures made by independent software, documented in
   `SOURCES.md`, plus round-trip and hostile-input tests.
3. Regenerate what is generated: `tools/gen-formats-doc` (after building the CLI),
   `tools/gen-notices`, `tools/sync-skills`.
4. Run `tools/privacy-audit --history`. Your commits must not contain personal or machine
   data (paths, user or host names, e-mail addresses, keys).
5. Commit with your GitHub noreply address (`git config user.email
   <id>+<login>@users.noreply.github.com`), a plain descriptive message and no trailers.

## Pull requests

Keep them focused; describe what changed and how you tested it, including the platforms you
built on. CI runs the core on Linux (GCC and Clang with sanitizers), macOS and Windows, the
web, Android and desktop apps, and the repository checks. A pull request is merged when CI is
green and a maintainer has reviewed it.

By contributing you agree that your contribution is licensed under the [MIT licence](LICENSE).
Test fixtures you add must be your own work, contributed without reserved rights.

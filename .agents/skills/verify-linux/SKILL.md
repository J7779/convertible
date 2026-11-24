---
name: verify-linux
description: Rehearse Convertible's Linux CI jobs locally in Docker without installing anything on the host - core with GCC 14, core with Clang 18 + libc++ under ASan/UBSan, the desktop app, the repository hygiene checks, and a MinGW-w64 cross-compile smoke check. Use after changing the core, CMake files, imported code or the desktop app, and when a Linux CI job fails.
---

# Verify on Linux (Docker)

`tools/ci/docker-linux.sh` runs each job in a throwaway container. It copies exactly the files
git would commit (no host build output) into the container and installs packages with
`tools/ci/install-linux-deps.sh`, the script the workflows use, so the command sequence is the
one CI runs. Docker must be running; run one job at a time.

## Steps

1. Pick the job(s):
   ```sh
   tools/ci/docker-linux.sh gcc        # ubuntu:24.04, GCC 14, dev preset, + gen-formats-doc --check
   tools/ci/docker-linux.sh clang      # ubuntu:24.04, Clang 18 + libc++ 18, asan preset
   tools/ci/docker-linux.sh desktop    # desktop build, desktop tests, .tar.gz/.deb packages
   tools/ci/docker-linux.sh hygiene    # privacy audit, skills mirror, notices, workflow YAML parse
   tools/ci/docker-linux.sh mingw      # debian:trixie MinGW-w64 GCC 14: core + CLI cross build
   ```
   Knobs: `CVT_DOCKER_MEMORY` (default `6g`), `CVT_JOBS` (default 6). A full core build plus
   tests takes roughly 10-15 minutes with 6 jobs; the sanitizer build is slower.
2. Read the result: the script ends with `==> <job>: passed` or exits non-zero. For core test
   failures, search the output for `failed test cases:`; for compile errors, for `error:`.
3. Try a fix for imported C code without editing CMake first by passing flags through:
   `CFLAGS=-D_DEFAULT_SOURCE tools/ci/docker-linux.sh gcc` (only `CFLAGS` is forwarded).
   Then put the real fix in the owning family's `family.cmake` and re-run without it.
4. Interactive debugging: `tools/ci/docker-linux.sh shell` gives a shell in the GCC environment
   with the tree copied to `/work/src`.

## Notes

- Differences from GitHub's runners: minimal image instead of the full runner image, the
  distribution's CMake 3.28, no compiler cache, memory capped by `--memory`.
- Ubuntu 24.04's MinGW is GCC 13 and cannot compile the core; the `mingw` job therefore uses
  Debian trixie (GCC 14). It is a compile/link smoke check only; MSVC is exercised in CI.
- The web job has its own container: `apps/web/build.sh --docker && apps/web/test.sh`.

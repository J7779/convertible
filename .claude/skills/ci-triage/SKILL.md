---
name: ci-triage
description: Diagnose a failing Convertible GitHub Actions run - find the failed job and step with the gh CLI, read only the failing log lines, classify the failure (compile, test, generated-file check, privacy audit, infrastructure), and reproduce the job locally with the same tools/ci script or in Docker before fixing. Use when CI is red or a workflow behaves unexpectedly.
---

# CI triage

Workflows: `.github/workflows/ci.yml`, `release.yml`, `pages.yml`. Every job step calls a
script in `tools/ci/` or a documented command, so each one can be reproduced locally.

## Steps

1. **Find the failure**:
   ```sh
   gh run list --workflow ci.yml --limit 5
   gh run view <run-id>                        # jobs and their conclusions
   gh run view <run-id> --log-failed           # only the failed steps' logs
   gh run view <run-id> --job <job-id> --log   # one whole job
   gh run download <run-id> -n test-logs-linux-gcc14   # uploaded test logs / packages
   ```
2. **Classify it** from the log:
   | Symptom | Where to look |
   | --- | --- |
   | `error:` while compiling | the file and compiler; platform-specific (MSVC, libc++, glibc, NDK)? |
   | `failed test cases:` | the listed `file:line: name`; re-run that case locally |
   | `gen-formats-doc: out of date` | a format changed: build the CLI, run `tools/gen-formats-doc`, commit |
   | `gen-notices` / `sync-skills` out of date | run the tool without `--check`, commit |
   | `privacy-audit` findings | the `privacy-audit` skill |
   | actionlint / shellcheck findings | the workflow file and line printed |
   | apt, sdkmanager, Docker pull or Gradle download errors, runner lost | infrastructure: re-run the job (`gh run rerun <run-id> --failed`) before changing code |
   | a build that passes locally but fails only after "Restore build directory" | a stale cached build directory: delete that cache (`gh cache list`, `gh cache delete <key>`) and re-run; report it, since `tools/ci/build-cache.sh` should make this impossible |
   | `The job running on runner ... has exceeded the maximum execution time` | a cold cache rebuilds the full core: re-run once (the cache is then warm); raise `timeout-minutes` only if a warm run is slow too |
3. **Reproduce locally** with the job's own command:
   | Job | Local command |
   | --- | --- |
   | core / linux-gcc14 | `tools/ci/docker-linux.sh gcc` |
   | core / linux-clang18-asan | `tools/ci/docker-linux.sh clang` |
   | core / macos-appleclang | `tools/ci/core.sh dev` on a Mac |
   | core / windows-msvc | `bash tools/ci/core.sh dev` in Git Bash on Windows with Visual Studio 2022 |
   | web | `apps/web/build.sh --docker && apps/web/test.sh` |
   | android | `cd apps/android && ./gradlew --max-workers=4 assembleDebug assembleRelease testDebugUnitTest lint` |
   | android / emulator | `tools/ci/android-emulator.sh up`, then `./gradlew connectedDebugAndroidTest` |
   | desktop / * | `tools/ci/desktop.sh` (Linux: `tools/ci/docker-linux.sh desktop`) |
   | hygiene | `tools/ci/docker-linux.sh hygiene`, or the four `tools/*` checks |
4. **Fix at the source** in the owning component (see ownership in `AGENTS.md`), re-run the
   local reproduction, then push. If the failure is in another owner's area, write down the
   exact error, the job, and a proposed fix for them instead of working around it in CI.
5. **Workflow changes**: lint before pushing:
   ```sh
   docker run --rm -v "$PWD:/repo:ro" -w /repo rhysd/actionlint:1.7.12 -color
   ```
   Keep actions to GitHub's own (`actions/*`) pinned to full commit SHAs with the version in a
   comment, minimal `permissions`, and a `timeout-minutes` on every job.

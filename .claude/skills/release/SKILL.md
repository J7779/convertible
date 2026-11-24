---
name: release
description: Cut a Convertible release - bump the single project version, tag v<version>, let .github/workflows/release.yml build the CLI, desktop, Android and web artefacts with checksums and publish the GitHub Release, then verify the artefacts. Use when preparing, dry-running or checking a release. Only the repository owner pushes tags.
---

# Release

## What the release workflow produces

`.github/workflows/release.yml` (tags `v*`, or by hand for a dry run):

| Job | Artefacts |
| --- | --- |
| desktop + cli / linux-x64 | `convertible-cli-<v>-linux-x64.tar.gz`, `convertible-desktop-<v>-linux-x64.tar.gz`, `convertible-desktop_<v>_amd64.deb` |
| desktop + cli / macos-universal | `convertible-cli-<v>-macos-universal.tar.gz`, `Convertible-<v>-macos-arm64-x64.zip` (unsigned) |
| desktop + cli / windows-x64 | `convertible-cli-<v>-windows-x64.zip`, `Convertible-<v>-windows-x64.zip` |
| android | `convertible-android-<v>-{arm64-v8a,armeabi-v7a,x86_64,universal}-{release-unsigned,debug}.apk` |
| web | `convertible-web-<v>.zip` |
| publish | `SHA256SUMS.txt`; on tags the GitHub Release with all files and notes |

There is no iOS artefact (signing needs the owner's certificate).

## Steps

1. **Green main**: the latest CI run on `main` passes (`gh run list --workflow ci.yml --branch main --limit 1`).
2. **Bump the version** in one place, the root `CMakeLists.txt`
   (`project(Convertible VERSION x.y.z ...)`); the CLI, desktop packages and Android
   `versionName`/`versionCode` follow it. Check: `tools/ci/version.sh`.
3. **Regenerate and audit**:
   ```sh
   cmake --preset release && cmake --build --preset release -j
   tools/gen-formats-doc --cli build/release/core/convertible
   tools/gen-notices
   tools/privacy-audit --history
   ```
   Commit as "Release x.y.z".
4. **Dry run** (optional): run the workflow by hand from the Actions tab or
   `gh workflow run release.yml --ref main`; it builds everything and uploads one workflow
   artifact `convertible-<v>` without publishing.
5. **Tag and push** (owner only): `git tag vX.Y.Z && git push origin vX.Y.Z`. The `version`
   job fails immediately if the tag does not equal `v` + the CMake version.
6. **Verify the artefacts** after the run:
   ```sh
   gh release download vX.Y.Z --dir /tmp/convertible-release
   cd /tmp/convertible-release && sha256sum -c SHA256SUMS.txt     # macOS: shasum -a 256 -c
   tar -xzf convertible-cli-X.Y.Z-linux-x64.tar.gz && ./convertible-cli-X.Y.Z-linux-x64/convertible --version
   dpkg-deb --info convertible-desktop_X.Y.Z_amd64.deb
   unzip -l Convertible-X.Y.Z-macos-arm64-x64.zip | head
   ```
   On a Mac: `lipo -archs` on the CLI shows `x86_64 arm64`; opening the app needs right-click >
   Open (Gatekeeper, unsigned). Install `...-universal-debug.apk` on a device with
   `adb install`. Serve the web zip locally with `node apps/web/tools/serve.mjs` pointed at the
   unpacked folder, or compare it with a fresh `apps/web/build.sh` output.
7. **Sign outside CI** if distributing further: Android release APKs with the owner's key
   (`apksigner sign --ks <keystore> <apk>`), macOS with the owner's Developer ID. Keys never
   enter the repository or its secrets.

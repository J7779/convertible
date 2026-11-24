---
name: make-fixtures
description: Produce test fixtures for Convertible format tests - tiny synthetic files written by independent, widely used tools inside a throwaway container, scrubbed of personal and machine metadata, verified, and documented in SOURCES.md with tool, version and exact command. Use whenever a test needs a real file to decode.
---

# Make test fixtures

Policy: `core/tests/data/README.md`. A decode fixture proves the reader against the real
world, so it must never come from Convertible's own encoder.

## Steps

1. **Decide what the file must show**: the variant (bit depth, colour type, codec profile,
   byte order, compression mode, old/new version) and the known content the test will check
   (pixel values, sample values, cells, text). Keep it tiny: ideally under 50 KB (8x8 to 64x64
   images, well under a second of audio, a few video frames, one-page documents).
2. **Generate it with an independent tool in a container**, never with software installed on
   your host, and with synthetic content only (patterns, tones, lorem ipsum, made-up tables):
   ```sh
   mkdir -p /tmp/fixtures && cd /tmp/fixtures
   docker run --rm --memory 6g -v "$PWD:/out" -w /out debian:trixie bash -c '
     apt-get update -qq && apt-get install -y -qq --no-install-recommends ffmpeg >/dev/null &&
     ffmpeg -hide_banner -f lavfi -i "testsrc2=size=32x32:rate=10" -frames:v 5 \
       -map_metadata -1 -bitexact -c:v libx264 -pix_fmt yuv420p sample_32x32.mp4'
   ```
   Record the image, the tool's exact version (`ffmpeg -version | head -1`) and the command.
3. **Scrub metadata** while generating (`-map_metadata -1 -bitexact`, `exiftool -all=`,
   `zip -X`, `tar --owner=0 --group=0 --numeric-owner --mtime=@0`, document author / company
   fields emptied) and check the bytes:
   ```sh
   strings -a sample_32x32.mp4 | grep -iE 'users|home|@|[a-z0-9-]+\.local' || echo clean
   ```
4. **Copy into the family's directory** `core/tests/data/<family>/` with a lower-case name that
   says what is inside (`sine_440hz_mono_8k.wav`). Never convert line endings: the directory is
   `-text` in `.gitattributes`.
5. **Document it** in `core/tests/data/<family>/SOURCES.md`: what it contains, the known values
   the tests rely on, tool and version, the exact command with relative paths only. See
   `core/tests/data/support/SOURCES.md` for the format. A fixture without an entry does not pass
   review.
6. **Check** that it is yours to give (created by you, no rights reserved; nothing downloaded or
   taken from another project's test suite) and run:
   ```sh
   tools/privacy-audit
   ```

# Sources: codec_av1

Tiny AV1 streams written by independent encoders, and reference MD5s of the
frames decoded by an independent decoder (dav1d). Used by
`core/tests/families/codec_av1/`. All content is synthetic (FFmpeg `testsrc`,
`testsrc2` test patterns, some with FFmpeg's deterministic `noise` filter).

Tools: Ubuntu 24.04 packages in a throwaway container — FFmpeg 6.1.1
(`libaom-av1` = libaom 3.8.2, `libsvtav1`, `librav1e`), `aomenc` 3.8.2 (aom-tools),
`dav1d` 1.4.1, Python 3. Commands run in this directory with a scratch
directory `tmp/`; `F="ffmpeg -hide_banner -loglevel error -y"`.

| File | Contents | Command |
| --- | --- | --- |
| `aom_8bit_420_64x64.ivf` | libaom, 64x64 4:2:0 8-bit, 20 frames at 30 fps; alt-ref: hidden frames and `show_existing_frame` | `$F -f lavfi -i testsrc2=size=64x64:rate=30 -frames:v 20 -pix_fmt yuv420p -c:v libaom-av1 -crf 32 -cpu-used 4 -auto-alt-ref 1 -lag-in-frames 19 -g 40 -map_metadata -1 -bitexact -f ivf aom_8bit_420_64x64.ivf` |
| `aom_10bit_420_96x64.ivf` | libaom, 96x64 4:2:0 10-bit, 10 frames | `$F -f lavfi -i testsrc2=size=96x64:rate=30 -frames:v 10 -pix_fmt yuv420p10le -c:v libaom-av1 -crf 30 -cpu-used 8 -map_metadata -1 -bitexact -f ivf aom_10bit_420_96x64.ivf` |
| `svt_10bit_420_grain_128x72.ivf` | SVT-AV1, 128x72 4:2:0 10-bit, 10 frames at 25 fps, film grain synthesis | `$F -f lavfi -i testsrc2=size=128x72:rate=25 -frames:v 10 -vf noise=alls=14:allf=t+u:all_seed=7 -pix_fmt yuv420p10le -c:v libsvtav1 -crf 35 -preset 10 -svtav1-params film-grain=10 -map_metadata -1 -bitexact -f ivf svt_10bit_420_grain_128x72.ivf` |
| `aom_8bit_420_grain_64x64.ivf` | libaom, 64x64 4:2:0 8-bit, 10 frames, film grain from libaom's denoiser | `$F -f lavfi -i testsrc2=size=64x64:rate=30 -frames:v 10 -vf noise=alls=14:allf=t+u:all_seed=7 -pix_fmt yuv420p -c:v libaom-av1 -crf 30 -cpu-used 8 -denoise-noise-level 30 -denoise-block-size 16 -map_metadata -1 -bitexact -f ivf aom_8bit_420_grain_64x64.ivf` |
| `rav1e_8bit_420_66x44.ivf` | rav1e, 66x44 4:2:0 8-bit, 10 frames at 24 fps | `$F -f lavfi -i testsrc2=size=66x44:rate=24 -frames:v 10 -pix_fmt yuv420p -c:v librav1e -speed 10 -qp 90 -map_metadata -1 -bitexact -f ivf rav1e_8bit_420_66x44.ivf` |
| `aom_8bit_420_odd_67x45.ivf` | libaom, odd size 67x45 4:2:0 8-bit, 10 frames at 24 fps | `$F -f lavfi -i testsrc=size=67x45:rate=24 -frames:v 10 -pix_fmt yuv420p -c:v libaom-av1 -crf 34 -cpu-used 8 -map_metadata -1 -bitexact -f ivf aom_8bit_420_odd_67x45.ivf` |
| `aom_8bit_444_tiles_176x144.ivf` | libaom, 176x144 4:4:4 8-bit (profile 1), 2x2 tiles, 10 frames | `$F -f lavfi -i testsrc2=size=176x144:rate=30 -frames:v 10 -pix_fmt yuv444p -c:v libaom-av1 -crf 40 -cpu-used 8 -tile-columns 1 -tile-rows 1 -map_metadata -1 -bitexact -f ivf aom_8bit_444_tiles_176x144.ivf` |
| `aom_12bit_422_64x48.ivf` | libaom, 64x48 4:2:2 12-bit (profile 2), 8 frames | `$F -f lavfi -i testsrc2=size=64x48:rate=30 -frames:v 8 -pix_fmt yuv422p12le -c:v libaom-av1 -crf 30 -cpu-used 8 -map_metadata -1 -bitexact -f ivf aom_12bit_422_64x48.ivf` |
| `aom_still_80x60.obu` | aomenc all-intra still picture (reduced still-picture header), 80x60 4:2:0 8-bit, Section 5 OBUs | `$F -f lavfi -i testsrc2=size=80x60:rate=1 -frames:v 1 -pix_fmt yuv420p -f yuv4mpegpipe -strict -1 tmp/still.y4m && aomenc --quiet --allintra --limit=1 --cpu-used=6 --end-usage=q --cq-level=20 --obu -o aom_still_80x60.obu tmp/still.y4m` |
| `aom_annexb_64x48.obu` | aomenc, Annex B (length-delimited) stream, 64x48 4:2:0 8-bit, 8 frames | `$F -f lavfi -i testsrc2=size=64x48:rate=30 -frames:v 8 -pix_fmt yuv420p -f yuv4mpegpipe -strict -1 tmp/annexb.y4m && aomenc --quiet --good --cpu-used=8 --end-usage=q --cq-level=30 --annexb=1 --obu -o aom_annexb_64x48.obu tmp/annexb.y4m` |
| `aom_mono_64x64.ivf` | aomenc, monochrome 64x64 8-bit, 8 frames | `$F -f lavfi -i testsrc2=size=64x64:rate=30 -frames:v 8 -pix_fmt yuv420p -f yuv4mpegpipe -strict -1 tmp/mono.y4m && aomenc --quiet --good --cpu-used=8 --end-usage=q --cq-level=30 --monochrome --ivf -o aom_mono_64x64.ivf tmp/mono.y4m` |

## Reference MD5s (`*.md5`)

One `.md5` per stream: a comment line, then the MD5 of every shown frame as
decoded by dav1d 1.4.1 (film grain applied, as the specification requires),
in the layout the Convertible decoder delivers: planes back to back without
row padding; samples above 8 bits as little-endian 16-bit words; 12-bit
samples rounded to 10 bits (`min((v + 2) >> 2, 1023)`); monochrome frames
followed by neutral 4:2:0 chroma planes (128, or 512 at 10 bits).

    dav1d -q -i aom_8bit_420_64x64.ivf -o tmp/aom_8bit_420_64x64.ivf.y4m      # likewise for every .ivf
    dav1d -q --demuxer section5 -i aom_still_80x60.obu -o tmp/aom_still_80x60.obu.y4m
    dav1d -q --demuxer annexb -i aom_annexb_64x48.obu -o tmp/aom_annexb_64x48.obu.y4m
    python3 refs.py tmp/<name>.y4m > <name>.md5

`refs.py` parses the YUV4MPEG2 header (`W`, `H`, `C420jpeg`/`C420p10`/
`C422p12`/`C444`/`Cmono` ...), splits the `FRAME` records, applies the
layout rules above and prints `hashlib.md5(frame).hexdigest()` per frame.

## Interoperability of the Convertible encoder (verified while developing)

Output of the Convertible AV1 encoder (320x240 synthetic moving content with
mild noise, 20 frames, written to IVF with temporal delimiters; stills as
Section 5 `.obu`) was decoded with dav1d 1.4.1, FFmpeg 6.1.1's libaom 3.8.2
decoder, and remuxed to Matroska with `ffmpeg -c copy`. Every frame of every
file was bit-identical (per-frame MD5) to the Convertible decoder's output:

| Stream | Bytes | PSNR worst / mean (dB) | dav1d | libaom (ffmpeg) |
| --- | --- | --- | --- | --- |
| 4:2:0 8-bit, quality 30 | 3822 | 38.85 / 40.52 | identical | identical |
| 4:2:0 8-bit, quality 50 | 5497 | 41.50 / 42.46 | identical | identical |
| 4:2:0 8-bit, quality 75 | 8681 | 41.89 / 43.57 | identical | identical |
| 4:2:0 8-bit, quality 90 | 12821 | 42.06 / 44.01 | identical | identical |
| 4:2:0 10-bit, quality 75 | 8555 | 42.06 / 43.91 | identical | identical |
| 4:4:4 8-bit, quality 75 | 10449 | 41.91 / 43.86 | identical | identical |
| 4:2:0 8-bit, quality 75, preset fast (real-time mode) | 25057 | 42.50 / 43.03 | identical | identical |
| still 333x251, quality 90 | 1831 | 43.98 | identical | — |
| still 333x251 4:4:4, lossless | 96186 | exact | identical | — |
| still 333x251 10-bit, quality 60 | 1404 | 41.41 | identical | — |
| still 333x251 monochrome (alpha), quality 80 | 1003 | — | identical | — |

(The source carries ±2 noise, which caps PSNR near 44 dB.)

## Speed of the generic C build (Release, one thread, Apple M-series)

| Task | Result |
| --- | --- |
| decode 1280x720 8-bit | about 50 frames/s |
| encode 1280x720, preset fast (libaom real-time, cpu-used 10) | about 17 frames/s |
| encode 1280x720, preset balanced (real-time, cpu-used 8) | about 12 frames/s |
| encode 1280x720, preset small (good quality, cpu-used 6, lag 19) | about 1.6 frames/s |
| encode 640x480, preset balanced (good quality, cpu-used 6, lag 19) | about 5 frames/s |
| encode 4000x3000 still, preset fast (all-intra, cpu-used 9) | 0.6 s |
| encode 4000x3000 still, preset balanced / small (all-intra, cpu-used 6) | 5.8 s |
| decode 4000x3000 still | 0.25–0.5 s |

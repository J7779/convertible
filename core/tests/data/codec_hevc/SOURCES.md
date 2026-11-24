# Sources: codec_hevc

H.265/HEVC streams written by three independent encoders (x265, Kvazaar and
Apple VideoToolbox), plus the per-frame MD5 checksums of ffmpeg's decoding of
each stream. The tests decode every stream with Convertible's decoder and
require the same MD5 for every frame (HEVC decoding is bit-exact). No file
was produced by Convertible.

All picture content is ffmpeg's synthetic `testsrc2` pattern (moving colour
bars, gradients and a counter). Encoder-information SEI messages were
disabled (`--no-info`) and nothing identifying is stored in any file.

## Tools

- ffmpeg 7.1.5 (Debian 13 package `ffmpeg` 7:7.1.5-0+deb13u1), in a throwaway
  `debian:trixie` container
- x265 4.1 (Debian 13 package `x265`, 8/10/12-bit build), same container
- Kvazaar 2.3.1 (Debian 13 package `kvazaar`), same container
- `avconvert` (AVFoundation, VideoToolbox HEVC encoder) on macOS 27.2
- `mp4extract.py`, a 120-line Python stdlib script that copies the hvcC record
  and the samples of the HEVC track of a QuickTime movie into the test
  container described below (not part of the repository; the format is
  simple enough to recreate)

## Raw source frames

Every x265 input is made with

    ffmpeg -f lavfi -i "testsrc2=size=WxH:rate=25" -frames:v N -pix_fmt PIXFMT -f yuv4mpegpipe -strict -1 SRC.y4m

with the size, frame count and pixel format given per file below.

## Streams (Annex-B elementary streams, `.h265`)

`X` stands for `x265 --no-info --log-level error`.

| File | Contents | Command |
| --- | --- | --- |
| `main_176x144_ctu64_bframes.h265` | Main, 176x144, 20 frames, 64x64 CTUs, 3 B-frames with pyramid, closed GOP | source 176x144, 20 frames, yuv420p; `X --input s176.y4m --preset medium --crf 30 --ctu 64 --bframes 3 --b-pyramid --keyint 20 --min-keyint 20 --no-scenecut -o main_176x144_ctu64_bframes.h265` |
| `main_64x64_ctu16_opengop.h265` | Main, 64x64, 12 frames, 16x16 CTUs, open GOP (CRA with leading pictures every 6 frames), B-frames | source 64x64, 12 frames, yuv420p; `X --input s64.y4m --preset fast --crf 26 --ctu 16 --bframes 2 --keyint 6 --open-gop -o main_64x64_ctu16_opengop.h265` |
| `main_128x96_ctu32_wpp.h265` | Main, 128x96, 12 frames, 32x32 CTUs, wavefront parallel processing (decoded single-threaded), P-frames with 3 references | source 128x96, 12 frames, yuv420p; `X --input s128.y4m --preset medium --crf 28 --ctu 32 --wpp --bframes 0 --ref 3 -o main_128x96_ctu32_wpp.h265` |
| `main_128x96_scaling_weighted.h265` | Main, 128x96, 12 frames: default scaling lists, weighted P/B prediction, transform skip, sign hiding, AMP and rectangular partitions, strong intra smoothing, deblocking offsets, SAO off | same source; `X --input s128.y4m --preset slow --crf 27 --ctu 32 --scaling-list default --weightp --weightb --tskip --signhide --amp --rect --strong-intra-smoothing --deblock 2:-1 --no-sao -o main_128x96_scaling_weighted.h265` |
| `main_128x96_lossless.h265` | Main, 128x96, 4 frames, lossless (transquant bypass) | same source; `X --input s128.y4m --preset fast --lossless --ctu 32 --bframes 0 --frames 4 -o main_128x96_lossless.h265` |
| `main_98x66_cropped.h265` | Main, coded at a multiple of the 8-pixel minimum CU and cropped to 98x66 by the conformance window, 10 frames | source 98x66, 10 frames, yuv420p; `X --input s98.y4m --preset medium --crf 28 --bframes 2 -o main_98x66_cropped.h265` |
| `main10_96x64.h265` | Main 10, 96x64, 10 frames, 10-bit 4:2:0, B-frames | source 96x64, 10 frames, yuv420p10le; `X --input s96p10.y4m --input-depth 10 --output-depth 10 --profile main10 --preset medium --crf 28 --bframes 2 -o main10_96x64.h265` |
| `mainstill_96x64.h265` | Main Still Picture, 96x64, one IDR picture (the HEIC case) | source 96x64, 2 frames, yuv420p; `X --input still.y4m --frames 1 --profile mainstillpicture --preset medium --crf 22 -o mainstill_96x64.h265` |
| `rext444_98x66.h265` | Main 4:4:4 (range extensions), 98x66 (x265 trims the 99x67 source to even sizes), 6 frames | source 99x67, 6 frames, yuv444p; `X --input s444.y4m --input-csp i444 --profile main444-8 --preset medium --crf 26 --bframes 0 -o rext444_98x66.h265` (file renamed from `rext444_99x67.h265`) |
| `rext422p10_96x64.h265` | Main 4:2:2 10, 96x64, 6 frames | source 96x64, 6 frames, yuv422p10le; `X --input s422.y4m --input-csp i422 --input-depth 10 --output-depth 10 --profile main422-10 --preset medium --crf 26 --bframes 0 -o rext422p10_96x64.h265` |
| `rext400_64x64.h265` | Monochrome (4:0:0, range extensions), 64x64, 6 frames | source 64x64, 6 frames, gray; `X --input gray.y4m --input-csp i400 --preset medium --crf 26 --bframes 0 -o rext400_64x64.h265` |
| `kvz_160x128_tiles.h265` | Main, 160x128, 8 frames, 2x2 tiles with one slice per tile, hierarchical GOP of 8 (Kvazaar) | source 160x128, 8 frames, converted to raw I420 with `ffmpeg -i s160.y4m -f rawvideo -pix_fmt yuv420p s160.yuv`; `kvazaar -i s160.yuv --input-res 160x128 -o kvz_160x128_tiles.h265 --tiles 2x2 --slices tiles --qp 30 --period 8 --gop 8 --no-info` |

## VideoToolbox samples (`.hvcs`)

| File | Contents | Command |
| --- | --- | --- |
| `vt_main10_176x144.hvcs` | Main 10, 176x144, 12 frames with B-frames, BT.709 colour description, from an Apple ProRes 422 source | `ffmpeg -f lavfi -i "testsrc2=size=176x144:rate=25" -frames:v 30 -c:v prores_ks -pix_fmt yuv422p10le -map_metadata -1 -bitexact vt_source.mov`; `avconvert -s vt_source.mov -p PresetHEVCHighestQuality -o vt_hevc.mov --duration 0.48`; `python3 mp4extract.py vt_hevc.mov vt_main10_176x144.hvcs` |
| `vt_main_160x96.hvcs` | Main (8-bit), 160x96, 12 frames | `ffmpeg -f lavfi -i "testsrc2=size=160x96:rate=25" -frames:v 12 -c:v libx264 -crf 8 -pix_fmt yuv420p -map_metadata -1 -bitexact vt8_source.mp4`; `avconvert -s vt8_source.mp4 -p PresetHEVCHighestQuality -o vt8_hevc.mov`; `python3 mp4extract.py vt8_hevc.mov vt_main_160x96.hvcs` |

The `.hvcs` test container (all integers big-endian): the ASCII tag `HVCS`,
a u32 hvcC size, the hvcC record (the payload of the `hvcC` box), a u32
sample count, then per sample a u32 size, an i32 PTS, an i32 DTS (both in the
movie's media timescale, 12800 per second) and the sample bytes exactly as
stored in the movie (4-byte length-prefixed NAL units).

## Reference checksums (`.md5`)

One line per frame in presentation order, after a comment line with the
picture size: the MD5 of the decoded planes as written by ffmpeg's
`framemd5` muxer (planes Y, Cb, Cr one after another, rows without padding,
16-bit little-endian samples for 10-bit streams; only Y for 4:0:0).

    ffmpeg -i NAME.h265 -f framemd5 NAME.framemd5     # .h265 streams
    ffmpeg -i vt_hevc.mov -f framemd5 vt_hevc.framemd5 # VideoToolbox movies

The `.framemd5` files were reduced to the hash column with a short Python
script. Convertible's decoder matches every frame of every stream exactly.

## Interoperability of Convertible's encoder output (not fixtures)

Checked while developing, with streams our encoder wrote from a 640x360,
48-frame ffmpeg `mandelbrot` clip at quality 30, 50, 75 and 90 (encoder
speeds fast, balanced and slow):

- ffmpeg 7.1.5 decodes every stream without warnings and its output is
  identical (per-frame MD5) to Convertible's decoder. Y-PSNR against the
  source, from ffmpeg's decoding: q30 32.15 dB, q50 33.63 dB, q75 36.01 dB,
  q90 40.80 dB (balanced); q90 fast 42.92 dB, q90 slow 42.42 dB.
- Remuxed into MP4 (`ffmpeg -r 25 -i x.h265 -c copy -tag:v hvc1 x.mp4`),
  AVFoundation/VideoToolbox (`avconvert -p PresetAppleProRes422LPCM`) decodes
  the streams; each of its pictures matches one of ffmpeg's at 50.5 dB (the
  ProRes intermediate), the order following the timestamps ffmpeg invented
  for the timestamp-less raw stream.

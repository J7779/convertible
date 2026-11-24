# Sources: codec_h264

H.264 streams written by independent encoders, with references decoded by
ffmpeg. All content is synthetic: ffmpeg's `testsrc2` test pattern (colour
bars, a moving gradient, a counter). No metadata: SEI messages are removed
from the x264 streams (x264 writes its settings there), container metadata is
dropped (`-map_metadata -1`, bitexact flags).

## Tools

- x264 fixtures: ffmpeg 6.1.1 (Ubuntu 24.04 package `ffmpeg 7:6.1.1-3ubuntu5`)
  with libx264 core 164 (`0.164.3108 31e19f9`), in a throwaway container.
  `tools/make_fixtures.sh` regenerates every one of them byte for byte (the
  header of the script shows the container command); run from this directory.
- VideoToolbox fixture: `avconvert` on macOS (Apple's VideoToolbox H.264
  encoder), from a ProRes source made by ffmpeg.
- `tools/mp4_to_pkt.py` (Python 3 standard library) extracts the video track
  of an MP4 into a `.pkt` file: `CVTPKT1\0`, u32 timescale, u32 + avcC record,
  u32 sample count, then per sample u32 size, i64 PTS, i64 DTS, u8 keyframe
  flag and the sample exactly as stored (4-byte length-prefixed NAL units).
  All integers are big-endian. These files give the decoder tests real avcC
  records and real container timestamps with B-frame reordering.
- `.md5` files: `ffmpeg -i <name>.h264 -f framemd5 <name>.md5`, one MD5 per
  decoded frame of the planar YUV output (I420 tightly packed). H.264 decoding
  is bit-exact, so the tests require exactly these hashes.

## Files

| File | Contents | x264 options (see `tools/make_fixtures.sh`) |
| --- | --- | --- |
| `baseline_cavlc_176x144.h264/.md5` | Constrained Baseline, CAVLC, 12 frames, IDR every 6 | `-profile:v baseline -crf 22 -g 6 -x264-params ref=1` |
| `main_cabac_bframes_176x144.h264/.md5/.pkt` | Main, CABAC, 3 B-frames with B-pyramid, 3 references, 16 frames | `-profile:v main -crf 22 -g 16 -bf 3 -x264-params b-pyramid=normal:ref=3:b-adapt=0` |
| `high_8x8_weighted_multiref_176x144.h264/.md5/.pkt` | High, 8x8 transform, explicit weighted P prediction, implicit weighted B, temporal direct, 4 references, 16 frames | `-profile:v high -crf 20 -x264-params 8x8dct=1:weightp=2:weightb=1:ref=4:bframes=2:b-pyramid=none:b-adapt=0:direct=temporal` |
| `high_cavlc_4slices_176x144.h264/.md5` | High with CAVLC, 4 slices per picture, B-frames, 10 frames | `-x264-params cabac=0:slices=4:bframes=1:ref=2` |
| `mbaff_176x144.h264/.md5/.pkt` | Interlaced (MBAFF), B-frames without implicit weighting, 10 frames | `-flags +ildct+ilme -x264-params tff=1:bframes=1:weightb=0` |
| `mbaff_implicit_weights_176x144.h264/.md5` | Interlaced (MBAFF), B-frames with implicit weighted bi-prediction, 10 frames | `-flags +ildct+ilme -x264-params tff=1:bframes=1:weightb=1` |
| `mbaff_implicit_weights_176x144.frames_1_3.yuv.zlib` | ffmpeg's decoding of frames 1 and 3 (the B pictures) of the stream above, raw I420, zlib-compressed | `ffmpeg -i mbaff_implicit_weights_176x144.h264 -f rawvideo`, frames 1 and 3, Python `zlib.compress(..., 9)` |
| `crop_98x58_fullrange_bt709.h264/.md5/.pkt` | 98x58 (coded 112x64, cropped), full range, BT.709 primaries/transfer/matrix, 30 fps VUI timing, B-frames, 10 frames | `-profile:v high -x264-params fullrange=on:colorprim=bt709:transfer=bt709:colormatrix=bt709:bframes=2` on `testsrc2=size=98x58:rate=30` |
| `crop_70x34_bt601.h264/.md5` | 70x34 (coded 80x48), Main, BT.601 (SMPTE 170M), 24 fps, 8 frames | `-profile:v main -x264-params colorprim=smpte170m:transfer=smpte170m:colormatrix=smpte170m:bframes=0:sar=4/3` on `testsrc2=size=70x34:rate=24` |
| `unsupported_high10_32x32.h264` | High 10 profile, 10-bit 4:2:0, 2 frames (the decoder must refuse it clearly) | `-pix_fmt yuv420p10le -profile:v high10 -crf 30` on 32x32 |
| `unsupported_high422_32x32.h264` | High 4:2:2 profile, 8-bit 4:2:2, 2 frames (must be refused clearly) | `-pix_fmt yuv422p -profile:v high422 -crf 30` on 32x32 |
| `videotoolbox_main_bframes_176x144.h264/.md5/.pkt` | Apple VideoToolbox: Main profile, CABAC, B-frames, BT.601 VUI, 12 frames | see below |

VideoToolbox fixture (the source movie is not committed):

```sh
ffmpeg -f lavfi -i testsrc2=size=176x144:rate=25 -frames:v 20 -c:v prores_ks -pix_fmt yuv422p10le \
       -map_metadata -1 -fflags +bitexact -flags:v +bitexact vt_source.mov
avconvert -s vt_source.mov -p Preset640x480 -o videotoolbox_main_bframes_176x144.mp4 --replace --duration 0.48
ffmpeg -i videotoolbox_main_bframes_176x144.mp4 -c:v copy -bsf:v h264_mp4toannexb \
       -map_metadata -1 -fflags +bitexact -flags:v +bitexact -f h264 videotoolbox_main_bframes_176x144.h264
python3 -I tools/mp4_to_pkt.py videotoolbox_main_bframes_176x144.mp4 videotoolbox_main_bframes_176x144.pkt
ffmpeg -i videotoolbox_main_bframes_176x144.h264 -f framemd5 videotoolbox_main_bframes_176x144.md5
```

## What the tests check

- Every frame of every supported stream decodes to exactly ffmpeg's output
  (MD5), as access-unit packets, as one packet holding the whole stream, and
  (`.pkt`) as MP4 samples with the avcC record, where the output timestamps
  must equal the container PTS in presentation order.
- `mbaff_implicit_weights_176x144`: libavc v1.9.0 decodes the I and P
  pictures exactly but the two B pictures differ slightly from ffmpeg (54 dB
  and 69 dB PSNR); the same stream without implicit weighting
  (`mbaff_176x144`) is exact. A test asserts exact frames elsewhere and at
  least 50 dB on those two. ffmpeg is used as the reference because it passes
  the JVT conformance streams for MBAFF implicit weighting and temporal direct.
- The avcC records built by `build_avcc()` from the parameter sets equal the
  records ffmpeg's MP4 muxer wrote, byte for byte.

## Interop of our encoder output

Streams written by our encoder (natural-like 320x240 test content, 30
frames, Main profile, CABAC, 2 B-frames) were decoded by ffmpeg 6.1.1 in the
same container with no errors or warnings (`ffmpeg -v error -i out.h264 -f
null -`), identified as Main profile, level 3.0, 30 frames, and compared with
the source by ffmpeg's `psnr` filter:

| quality | size | ffmpeg PSNR average / worst frame |
| --- | --- | --- |
| 100 | 688 568 B | 49.14 / 48.02 dB |
| 90 | 33 589 B | 40.94 / 40.32 dB |
| 75 | 11 018 B | 40.72 / 40.20 dB |
| 50 | 7 981 B | 39.37 / 38.36 dB |
| 20 | 4 967 B | 35.28 / 34.38 dB |
| 90 with `bitrate_kbps` 300 | 50 638 B (338 kbit/s) | 40.92 / 40.19 dB |

(The content carries fresh random grain of about +-4 levels in every frame,
which caps PSNR near 41 dB until the quantiser is fine enough to code the
grain itself.)

ffmpeg's PSNR matches the PSNR our own decoder measures on the same streams.

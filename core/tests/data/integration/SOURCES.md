# Integration fixtures

Synthetic content only: FFmpeg's `testsrc` pattern (64x48, 10 fps, 0.5 s =
5 frames) and a 440 Hz sine at 8 kHz mono. Made with FFmpeg 6.1.1 (Ubuntu
24.04 package) in a throwaway container; no metadata is written
(`-map_metadata -1` and the bitexact flags).

```sh
F="ffmpeg -hide_banner -loglevel error -y"
BE="-map_metadata -1 -fflags +bitexact -flags:v +bitexact -flags:a +bitexact"
V="-f lavfi -i testsrc=size=64x48:rate=10:duration=0.5"
A="-f lavfi -i sine=frequency=440:sample_rate=8000:duration=0.5"
```

| File | Content | Command |
| --- | --- | --- |
| `h264_alac.mp4` | H.264 (B-frames) + Apple Lossless | `$F $V $A -c:v libx264 -preset veryfast -crf 20 -bf 2 -pix_fmt yuv420p -c:a alac $BE h264_alac.mp4` |
| `prores_pcm.mov` | ProRes 422 (standard) + PCM | `$F $V $A -c:v prores_ks -profile:v 2 -c:a pcm_s16le $BE prores_pcm.mov` |
| `ffv1_flac.mkv` | FFV1 version 3, all intra + FLAC | `$F $V $A -c:v ffv1 -level 3 -g 1 -c:a flac $BE ffv1_flac.mkv` |
| `huffyuv_pcm.avi` | HuffYUV + PCM | `$F $V $A -c:v huffyuv -c:a pcm_s16le $BE huffyuv_pcm.avi` |
| `ref_testsrc_64x48_i420.yuv.gz` | the 5 source frames as raw I420, gzip | `$F $V -pix_fmt yuv420p -f rawvideo ref_testsrc_64x48_i420.yuv && gzip -9 -n ref_testsrc_64x48_i420.yuv` |

The tests also use fixtures of other families (`video/`, `codec_hevc/`,
`codec_legacy/`, `image/gif/`, `pdf/photo.jpg`); see their SOURCES.md.

## Our output checked with independent tools

While developing, the CLI converted the fixtures and FFmpeg 6.1.1 checked
every result in a container (`ffprobe -count_frames` for streams and frame
counts, `ffmpeg -v error -xerror -i out -f null -` for a clean decode). All
decoded without errors, with the expected streams and frame counts:

| Our output | ffprobe |
| --- | --- |
| `h264_alac.mp4` -> MKV -> MOV (stream copy) | h264 64x48 5 frames + alac |
| MOV -> WebM (`streams=video_only`) | vp9 64x48 5 frames |
| WebM -> AVI | mjpeg 64x48 5 frames |
| AVI -> MPEG-TS -> MP4 | h264 64x48 5 frames |
| `video/h264_aac.mp4` -> MKV HEVC, MP4 AV1, MP4 VP9 | hevc / av1 / vp9 64x64 10 frames |
| `codec_hevc/main_176x144_ctu64_bframes.h265` -> MP4 | hevc 176x144 20 frames (B-frame order) |
| `prores_pcm.mov` -> MKV FFV1, AVI HuffYUV | ffv1 / huffyuv 64x48 5 frames + pcm_s16le |
| `h264_alac.mp4` -> MOV ProRes (quality 95), AVI MPEG-4, OGV Theora, TS MPEG-2 | prores / mpeg4 / theora / mpeg2video, 5 frames |
| `h264_alac.mp4` -> GIF, APNG | gif / apng 64x48 5 frames |
| `h264_alac.mp4` -> WAV, FLAC | pcm_s16le / flac, 8 kHz mono |
| `video/h264.h264` -> MP4, `mpeg2_mp2.ts` -> MKV, `theora_vorbis.ogv` -> MKV (copy) | h264 / mpeg2video / theora + vorbis, 10 frames |

One defect found this way and fixed: AVI stream format chunks of odd size
were not padded to an even length (FFmpeg warned about a misread `indx`).

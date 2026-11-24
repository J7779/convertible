# Video fixtures

All files are synthetic: FFmpeg's `testsrc` test pattern (64x64 unless noted, 10 frames per
second, 1 second) and `sine` tone (440 Hz, amplitude 1/8, mono, 8 kHz unless noted). They
were produced in a throwaway `ubuntu:24.04` container with the distribution packages
`ffmpeg` (6.1.1-3ubuntu5), `mkvtoolnix` (mkvmerge v82.0) and `python3` (3.12.3).
No file carries metadata beyond what the muxers write with `-bitexact`.

Shorthands used in the commands below (exactly as in the generator script):

```
F   = ffmpeg -hide_banner -loglevel error -y
BE  = -map_metadata -1 -fflags +bitexact -flags:v +bitexact -flags:a +bitexact
V   = -f lavfi -i testsrc=size=64x64:rate=10:duration=1
A   = -f lavfi -i sine=frequency=440:sample_rate=8000:duration=1
A16 = -f lavfi -i sine=frequency=440:sample_rate=16000:duration=1
A48 = -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1
X264 = -c:v libx264 -preset veryfast -pix_fmt yuv420p -g 10 -bf 2 -x264-params bframes=2:b-pyramid=0:scenecut=0
```

## Reference tables (`probe/`)

For every container fixture `<name>`, `probe/<name>.streams.csv` and `probe/<name>.packets.csv`
were derived from

```
ffprobe -v error -show_format -show_streams -show_packets -show_data_hash CRC32 -of json <name>
```

(streams: index, type, codec, tag, size, sample rate, channels, time base, extradata size and
CRC-32; packets in file order: stream, pts, dts, duration, size, flags K/D, payload CRC-32).
The demuxer tests compare every packet against these independent values.

## Containers

| File | Content | Command |
| --- | --- | --- |
| `h264_aac.mp4` | H.264 High with B-frames (edit list), AAC-LC mono 8 kHz with priming | `$F $V $A $X264 -c:a aac -b:a 24k -ac 1 $BE h264_aac.mp4` |
| `h264_aac_frag.mp4` | the same, fragmented | as above plus `-movflags frag_keyframe+empty_moov+default_base_moof` |
| `h264_aac_large_mdat.mp4` | `h264_aac.mp4` with a 64-bit (`largesize`) `mdat` header and adjusted `stco` | `python3 edge.py` (below) |
| `h264_aac_cmov.mov` | `h264_aac.mp4` with a zlib-compressed QuickTime movie header (`cmov`) | `python3 edge.py` (below) |
| `hevc.mp4` | HEVC (`hvc1`) | `$F $V -c:v libx265 -preset ultrafast -pix_fmt yuv420p -tag:v hvc1 -x265-params log-level=error:bframes=0 $BE hevc.mp4` |
| `av1.mp4` | AV1 | `$F $V -c:v libaom-av1 -cpu-used 8 -crf 40 -g 10 $BE av1.mp4` |
| `h264_rot90.mov` | H.264, display matrix rotated 90 degrees counter-clockwise | `$F $V $X264 -an $BE rot_src.mp4` then `$F -display_rotation 90 -i rot_src.mp4 -c copy $BE h264_rot90.mov` |
| `mjpeg_pcm.mov` | Motion JPEG + 16-bit PCM (`sowt`) | `$F $V $A -c:v mjpeg -q:v 5 -c:a pcm_s16le $BE mjpeg_pcm.mov` |
| `mpeg4_aac.m4v` | MPEG-4 Part 2 + AAC | `$F $V $A -c:v mpeg4 -q:v 5 -c:a aac -b:a 24k $BE mpeg4_aac.m4v` |
| `h263_aac.3gp` | H.263 128x96 + AAC | `$F $V $A -c:v h263 -s 128x96 -c:a aac -b:a 24k -ac 1 $BE h263_aac.3gp` |
| `two_audio.mp4` | H.264 + AAC 8 kHz + AAC 16 kHz | `$F $V $A $A16 -map 0:v -map 1:a -map 2:a $X264 -c:a aac -b:a 24k $BE two_audio.mp4` |
| `aac.m4a` | AAC only | `$F $A -c:a aac -b:a 24k $BE aac.m4a` |
| `vfr.mp4` | H.264 with variable frame timing (0, 100, 200, 300, 350, 400, 550 ... ms) | `$F $V -vf "setpts='if(lt(N,3),N*0.1,0.3+(N-3)*0.05+if(gt(N,5),0.1,0))/TB'" -fps_mode passthrough $X264 -enc_time_base 1/1000 $BE vfr.mp4` |
| `h264_vorbis.mkv` | H.264 + Vorbis | `$F $V $A $X264 -c:a libvorbis -q:a 2 $BE h264_vorbis.mkv` |
| `vp9_opus.webm` | VP9 + Opus 48 kHz (CodecDelay) | `$F $V $A48 -c:v libvpx-vp9 -deadline realtime -b:v 100k -g 10 -c:a libopus -b:a 24k $BE vp9_opus.webm` |
| `vp8_vorbis.webm` | VP8 + Vorbis | `$F $V $A -c:v libvpx -deadline realtime -b:v 100k -g 10 -c:a libvorbis -q:a 2 $BE vp8_vorbis.webm` |
| `av1.webm` | AV1 | `$F $V -c:v libaom-av1 -cpu-used 8 -crf 40 -g 10 $BE av1.webm` |
| `mpeg4_flac_pcm.mkv` | MPEG-4 Part 2 + FLAC 8 kHz + PCM 16 kHz | `$F $V $A $A16 -map 0:v -map 1:a -map 2:a -c:v mpeg4 -q:v 5 -c:a:0 flac -c:a:1 pcm_s16le $BE mpeg4_flac_pcm.mkv` |
| `mkvmerge_h264.mkv` | `h264_aac.mp4` remuxed by mkvmerge (no lacing) | `mkvmerge -q --disable-lacing -o mkvmerge_h264.mkv h264_aac.mp4` |
| `mkvmerge_lacing.mka` | `vp8_vorbis.webm` remuxed by mkvmerge with its default Xiph lacing (the trailing `-D` applies to no file, so the VP8 track is kept) | `mkvmerge -q -o mkvmerge_lacing.mka vp8_vorbis.webm -D` |
| `mpeg4_mp3.avi` | MPEG-4 Part 2 (`XVID`) + MP3 | `$F $V $A -c:v mpeg4 -q:v 5 -vtag XVID -c:a libmp3lame -b:a 32k $BE mpeg4_mp3.avi` |
| `mjpeg_pcm.avi` | Motion JPEG + PCM | `$F $V $A -c:v mjpeg -q:v 5 -c:a pcm_s16le $BE mjpeg_pcm.avi` |
| `rawvideo_i420.avi` | uncompressed I420, 5 frames (OpenDML `indx`/`ix00` and `idx1`) | `$F $V -frames:v 5 -c:v rawvideo -pix_fmt yuv420p $BE rawvideo_i420.avi` |
| `h264_pcmu8.avi` | H.264 with B-frames (no stored presentation times) + 8-bit PCM | `$F $V $A $X264 -c:a pcm_u8 $BE h264_pcmu8.avi` |
| `h264_aac.flv` | H.264 + AAC | `$F $V $A $X264 -c:a aac -b:a 24k $BE h264_aac.flv` |
| `flv1_mp3.flv` | Sorenson Spark + MP3 11 kHz | `$F $V $A -c:v flv1 -c:a libmp3lame -ar 11025 -b:a 32k $BE flv1_mp3.flv` |
| `h264_aac.ts` | MPEG-TS, H.264 + AAC (ADTS) | `$F $V $A $X264 -c:a aac -b:a 24k -mpegts_flags +initial_discontinuity $BE h264_aac.ts` |
| `mpeg2_mp2.ts` | MPEG-TS, MPEG-2 video + MP2 16 kHz | `$F $V $A -c:v mpeg2video -q:v 5 -c:a mp2 -ar 16000 -b:a 64k $BE mpeg2_mp2.ts` |
| `hevc_ac3.ts` | MPEG-TS, HEVC + AC-3 32 kHz | `$F $V $A -c:v libx265 -preset ultrafast -x265-params log-level=error -c:a ac3 -ar 32000 $BE hevc_ac3.ts` |
| `mpeg2_mp2.vob` | MPEG-2 program stream (DVD packs) | `$F $V $A -c:v mpeg2video -q:v 5 -c:a mp2 -ar 16000 -b:a 64k -f vob $BE mpeg2_mp2.vob` |
| `mpeg1_mp2.mpg` | MPEG-1 program stream, 25 fps, 0.4 s | `$F -f lavfi -i testsrc=size=64x64:rate=25:duration=0.4 $A -c:v mpeg1video -q:v 5 -c:a mp2 -ar 16000 -b:a 64k -f mpeg $BE mpeg1_mp2.mpg` |
| `theora_vorbis.ogv` | Ogg, Theora + Vorbis | `$F $V $A -c:v libtheora -q:v 5 -g 10 -c:a libvorbis -q:a 2 $BE theora_vorbis.ogv` |
| `opus.ogg` | Ogg Opus 48 kHz | `$F $A48 -c:a libopus -b:a 24k $BE opus.ogg` |
| `flac.oga` | Ogg FLAC | `$F $A -c:a flac $BE flac.oga` |
| `wmv2_wmav2.wmv` | ASF, WMV2 + WMA v2 | `$F $V $A -c:v wmv2 -q:v 5 -c:a wmav2 -b:a 32k $BE wmv2_wmav2.wmv` |
| `wmav2.wma` | ASF, WMA v2 only | `$F $A -c:a wmav2 -b:a 32k $BE wmav2.wma` |
| `vp8.ivf`, `vp9.ivf`, `av1.ivf` | IVF | `$F $V -c:v libvpx -deadline realtime -b:v 100k -g 10 $BE vp8.ivf` (likewise `libvpx-vp9`, and `libaom-av1 -cpu-used 8 -crf 40 -g 10`) |
| `h264.h264` | raw Annex-B H.264 (same settings as `h264_aac.mp4`) | `$F $V $X264 -bsf:v h264_mp4toannexb -f h264 $BE h264.h264` |
| `hevc.h265` | raw Annex-B HEVC | `$F $V -c:v libx265 -preset ultrafast -x265-params log-level=error -f hevc $BE hevc.h265` |
| `av1.obu` | raw AV1 low-overhead OBUs | `$F $V -c:v libaom-av1 -cpu-used 8 -crf 40 -g 10 -f obu $BE av1.obu` |
| `mpeg2.m2v` | raw MPEG-2 video | `$F $V -c:v mpeg2video -q:v 5 -f mpeg2video $BE mpeg2.m2v` |
| `i420.y4m` | YUV4MPEG2 4:2:0, 5 frames | `$F $V -frames:v 5 -pix_fmt yuv420p -f yuv4mpegpipe $BE i420.y4m` |
| `i444.y4m` | YUV4MPEG2 4:4:4, 32x24, 10 frames | `$F -f lavfi -i testsrc=size=32x24:rate=10:duration=1 -pix_fmt yuv444p -f yuv4mpegpipe -strict -1 $BE i444.y4m` |
| `rv10_ac3.rm` | RealMedia, RealVideo 1.0 + AC-3 (`dnet`) 44.1 kHz | `$F $V $A -c:v rv10 -q:v 5 -c:a ac3 -ar 44100 -b:a 64k $BE rv10_ac3.rm` |

## Reference content

| File | Content | Command |
| --- | --- | --- |
| `ref_testsrc_64x64_i420.yuv.gz` | the 10 decoded `testsrc` frames as raw I420 (6144 bytes each), gzip | `$F $V -pix_fmt yuv420p -f rawvideo ref.yuv && gzip -9 -n -c ref.yuv > ref_testsrc_64x64_i420.yuv.gz` |
| `ref_sine440_8k_s16le.pcm.gz` | the 8000 samples of the tone as signed 16-bit PCM, gzip | `$F $A -f s16le -c:a pcm_s16le ref.pcm && gzip -9 -n -c ref.pcm > ref_sine440_8k_s16le.pcm.gz` |

## `edge.py`

Python standard library only. Reads `h264_aac.mp4` (whose `moov` follows `mdat`) and writes:

- `h264_aac_large_mdat.mp4`: the `mdat` box rewritten with `size = 1` and a 64-bit
  `largesize`, every `stco` entry increased by 8;
- `h264_aac_cmov.mov`: the `moov` box replaced by `moov { cmov { dcom 'zlib', cmvd { u32
  uncompressed size, zlib.compress(original moov, 9) } } }`.

## Interoperability of our output

Files written by the `convertible` CLI from these fixtures (55 remux combinations across
MP4/MOV/3GP/M4A, Matroska/WebM/MKA, AVI, FLV, MPEG-TS, Ogg, IVF, Y4M, raw H.264/HEVC,
MP3, ADTS AAC, FLAC and WAV, plus raw-video and PCM encodes) were checked in the same
container with `ffprobe -show_streams -show_packets` (stream layout, packet counts,
rotation side data) and fully decoded with `ffmpeg -v error -i <file> -f null -`: all
decode without errors. The only messages came from ffmpeg's own timestamp guessing for
Matroska files holding H.264 B-frames with variable frame timing, which ffmpeg also prints
for its own remux of the same stream.

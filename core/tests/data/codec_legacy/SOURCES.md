# codec_legacy fixtures

Every file was made with `tools/make_fixtures.sh` (run it from this directory in a throwaway
Ubuntu 24.04 container with ffmpeg 6.1.1 (built with libtheora and libxvid) and python3; see the
comment at the top of the script). Content is synthetic: ffmpeg's `testsrc2` pattern only, no
metadata (`-map_metadata -1 -fflags +bitexact -flags:v +bitexact`). Re-running the script
reproduces every file byte for byte.

For each clip the script runs, with `<name>` and the arguments below:

```sh
ffmpeg -hide_banner -loglevel error -y <input and encoder arguments> -map_metadata -1 -fflags +bitexact -flags:v +bitexact tmp/<name>.<ext>
python3 tools/fixture_tool.py pkt tmp/<name>.<ext> <name>.pkt <codec id> [pkt options]
ffmpeg -hide_banner -loglevel error -y -i tmp/<name>.<ext> -fps_mode passthrough -f rawvideo -pix_fmt <reference pixel format> tmp/<name>.raw
python3 tools/fixture_tool.py ref tmp/<name>.raw <name>.ref <reference pixel format> <width> <height> [ref options]
```

* `<name>.pkt` (CVTPKT01 / CVTPKTZ1): every packet of the video stream with its timestamps,
  key flag and the stream extradata, as a demuxer would deliver them (`fixture_tool.py` documents
  the layout; `tests/families/codec_legacy/legacy_test_support.hpp` reads it).
* `<name>.ref` (CVTREF01): ffmpeg's own decode of the same file - the stored frames as
  zlib-compressed planes (`--frames=` lists which), or an MD5 per frame (`--md5`, for the
  bit-exact codecs).
* `--divx-packed=5` rewrites the stream into a DivX "packed bitstream" (a P-VOP and the following
  B-VOP in one packet, then a not-coded VOP), `--xiph` stores Theora headers Xiph-laced, and
  `--mov-palette` stores the QuickTime colour table as 256 B,G,R,0 entries in the extradata.

| file | codec id | container | reference | size | encoder arguments | pkt / ref options |
| --- | --- | --- | --- | --- | --- | --- |
| `mpeg1_96x64.pkt`, `mpeg1_96x64.ref` | mpeg1video | mkv | yuv420p | 96x64 | `-f lavfi -i testsrc2=s=96x64:r=25 -frames:v 12 -c:v mpeg1video -q:v 5 -bf 2 -g 9` | --frames=0,1,5,11 |
| `mpeg2_prog_96x64.pkt`, `mpeg2_prog_96x64.ref` | mpeg2video | mkv | yuv420p | 96x64 | `-f lavfi -i testsrc2=s=96x64:r=25 -frames:v 12 -c:v mpeg2video -q:v 5 -bf 2 -g 9` | --frames=0,2,4,11 |
| `mpeg2_interlaced_96x64.pkt`, `mpeg2_interlaced_96x64.ref` | mpeg2video | mkv | yuv420p | 96x64 | `-f lavfi -i testsrc2=s=96x64:r=25 -frames:v 12 -c:v mpeg2video -q:v 5 -qmax 28 -bf 2 -g 9 -flags +ildct+ilme -top 1 -alternate_scan 1 -intra_vlc 1 -non_linear_quant 1 -dc 10` | --frames=0,1,6,11 |
| `mpeg2_422_matrix_64x64.pkt`, `mpeg2_422_matrix_64x64.ref` | mpeg2video | mkv | yuv422p | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 8 -c:v mpeg2video -pix_fmt yuv422p -q:v 4 -bf 1 -g 6 -intra_matrix 8,16,19,22,26,27,29,34,16,16,22,24,27,29,34,37,19,22,26,27,29,34,34,38,22,22,26,27,29,34,37,40,22,26,27,29,32,35,40,48,26,27,29,32,35,40,48,58,26,27,29,34,38,46,56,69,27,29,35,38,46,56,69,83 -inter_matrix 20,17,18,19,20,21,22,23,17,18,19,20,21,22,23,24,18,19,20,21,22,23,24,25,19,20,21,22,23,24,26,27,20,21,22,23,25,26,27,28,21,22,23,24,26,27,28,30,22,23,24,26,27,28,30,31,23,24,25,27,28,30,31,33` | --frames=0,1,7 |
| `mpeg2_long_gop_48x48.pkt`, `mpeg2_long_gop_48x48.ref` | mpeg2video | mkv | yuv420p | 48x48 | `-f lavfi -i testsrc2=s=48x48:r=25 -frames:v 120 -c:v mpeg2video -q:v 3 -bf 0 -g 600 -sc_threshold 1000000000` | --frames=0,119 |
| `mpeg4_asp_packed_96x64.pkt`, `mpeg4_asp_packed_96x64.ref` | mpeg4 | avi | yuv420p | 96x64 | `-f lavfi -i testsrc2=s=96x64:r=25 -frames:v 12 -c:v mpeg4 -vtag XVID -q:v 5 -bf 2 -g 30 -flags +qpel+mv4` | --divx-packed=5 --frames=0,2,6,11 |
| `mpeg4_global_header_96x64.pkt`, `mpeg4_global_header_96x64.ref` | mpeg4 | mkv | yuv420p | 96x64 | `-f lavfi -i testsrc2=s=96x64:r=25 -frames:v 10 -c:v mpeg4 -q:v 5 -bf 1 -g 12 -mpeg_quant 1` | --frames=0,1,9 |
| `mpeg4_xvid_gmc_96x64.pkt`, `mpeg4_xvid_gmc_96x64.ref` | mpeg4 | avi | yuv420p | 96x64 | `-f lavfi -i testsrc2=s=96x64:r=25 -frames:v 10 -c:v libxvid -q:v 5 -bf 2 -gmc 1 -flags +qpel` | --frames=0,1,5,9 |
| `mpeg4_interlaced_96x64.pkt`, `mpeg4_interlaced_96x64.ref` | mpeg4 | avi | yuv420p | 96x64 | `-f lavfi -i testsrc2=s=96x64:r=25 -frames:v 8 -c:v mpeg4 -q:v 5 -bf 1 -flags +ildct+ilme` | --frames=0,1,7 |
| `mpeg4_sp_partitioned_96x64.pkt`, `mpeg4_sp_partitioned_96x64.ref` | mpeg4 | avi | yuv420p | 96x64 | `-f lavfi -i testsrc2=s=96x64:r=25 -frames:v 8 -c:v mpeg4 -q:v 5 -bf 0 -data_partitioning 1 -ps 120` | --frames=0,7 |
| `mpeg4_long_gop_48x48.pkt`, `mpeg4_long_gop_48x48.ref` | mpeg4 | avi | yuv420p | 48x48 | `-f lavfi -i testsrc2=s=48x48:r=25 -frames:v 120 -c:v mpeg4 -q:v 3 -bf 0 -g 600 -sc_threshold 1000000000` | --frames=0,119 |
| `h263_128x96.pkt`, `h263_128x96.ref` | h263 | 3gp | yuv420p | 128x96 | `-f lavfi -i testsrc2=s=128x96:r=25 -frames:v 8 -c:v h263 -q:v 5 -g 12` | --frames=0,7 |
| `h263p_umv_160x120.pkt`, `h263p_umv_160x120.ref` | h263 | avi | yuv420p | 160x120 | `-f lavfi -i testsrc2=s=160x120:r=25 -frames:v 8 -c:v h263p -q:v 5 -umv 1 -structured_slices 0` | --frames=0,7 |
| `flv1_64x64.pkt`, `flv1_64x64.ref` | flv1 | flv | yuv420p | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 10 -c:v flv -q:v 5 -g 12` | --frames=0,9 |
| `theora_64x64.pkt`, `theora_64x64.ref` | theora | mkv | yuv420p | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 12 -c:v libtheora -q:v 6 -g 8` | --xiph --md5 |
| `prores_p0_64x64.pkt`, `prores_p0_64x64.ref` | prores | mov | yuv422p10le | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v prores_ks -profile:v 0 -pix_fmt yuv422p10le` | --frames=1 |
| `prores_p1_64x64.pkt`, `prores_p1_64x64.ref` | prores | mov | yuv422p10le | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v prores_ks -profile:v 1 -pix_fmt yuv422p10le` | --frames=1 |
| `prores_p2_64x64.pkt`, `prores_p2_64x64.ref` | prores | mov | yuv422p10le | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v prores_ks -profile:v 2 -pix_fmt yuv422p10le` | --frames=1 |
| `prores_p3_64x64.pkt`, `prores_p3_64x64.ref` | prores | mov | yuv422p10le | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le` | --frames=1 |
| `prores_4444_alpha_64x64.pkt`, `prores_4444_alpha_64x64.ref` | prores | mov | yuva444p10le | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25,format=yuva444p10le,geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='16*X' -frames:v 2 -c:v prores_ks -profile:v 4 -pix_fmt yuva444p10le` | --frames=1 |
| `prores_interlaced_64x64.pkt`, `prores_interlaced_64x64.ref` | prores | mov | yuv422p10le | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 1 -c:v prores_ks -profile:v 3 -flags +ildct -pix_fmt yuv422p10le` | - |
| `huffyuv_yuv422_left_64x64.pkt`, `huffyuv_yuv422_left_64x64.ref` | huffyuv | avi | yuv422p | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 3 -c:v huffyuv -pix_fmt yuv422p -pred left` | --md5 |
| `huffyuv_yuv422_median_64x64.pkt`, `huffyuv_yuv422_median_64x64.ref` | huffyuv | avi | yuv422p | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 3 -c:v huffyuv -pix_fmt yuv422p -pred median` | --md5 |
| `huffyuv_rgb24_64x64.pkt`, `huffyuv_rgb24_64x64.ref` | huffyuv | avi | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 3 -c:v huffyuv -pix_fmt rgb24 -pred left` | --md5 |
| `huffyuv_bgra_64x64.pkt`, `huffyuv_bgra_64x64.ref` | huffyuv | avi | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 3 -c:v huffyuv -pix_fmt bgra -pred plane` | --md5 |
| `ffv1_v1_yuv420_64x64.pkt`, `ffv1_v1_yuv420_64x64.ref` | ffv1 | avi | yuv420p | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 3 -c:v ffv1 -level 1 -coder 0 -pix_fmt yuv420p` | --md5 |
| `ffv1_v3_yuv420_slices_64x64.pkt`, `ffv1_v3_yuv420_slices_64x64.ref` | ffv1 | mkv | yuv420p | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 3 -c:v ffv1 -level 3 -coder 1 -context 1 -slices 4 -slicecrc 1 -pix_fmt yuv420p` | --md5 |
| `ffv1_v3_rgb_64x64.pkt`, `ffv1_v3_rgb_64x64.ref` | ffv1 | mkv | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v ffv1 -level 3 -coder 1 -pix_fmt bgr0` | --md5 |
| `ffv1_v3_yuv422p10_64x64.pkt`, `ffv1_v3_yuv422p10_64x64.ref` | ffv1 | mkv | yuv422p10le | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v ffv1 -level 3 -coder 2 -pix_fmt yuv422p10le` | --md5 |
| `utvideo_yuv420_64x64.pkt`, `utvideo_yuv420_64x64.ref` | utvideo | avi | yuv420p | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v utvideo -pix_fmt yuv420p -pred left` | --md5 |
| `utvideo_yuv422_median_64x64.pkt`, `utvideo_yuv422_median_64x64.ref` | utvideo | avi | yuv422p | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v utvideo -pix_fmt yuv422p -pred median` | --md5 |
| `utvideo_rgb_median_64x64.pkt`, `utvideo_rgb_median_64x64.ref` | utvideo | avi | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v utvideo -pix_fmt gbrp -pred median` | --md5 |
| `utvideo_rgba_64x64.pkt`, `utvideo_rgba_64x64.ref` | utvideo | avi | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 2 -c:v utvideo -pix_fmt gbrap -pred left` | --md5 |
| `cinepak_64x64.pkt`, `cinepak_64x64.ref` | cinepak | avi | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 5 -c:v cinepak -pix_fmt rgb24` | --md5 |
| `msrle_64x64.pkt`, `msrle_64x64.ref` | msrle | avi | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 5 -c:v msrle -pix_fmt pal8` | --md5 |
| `msvideo1_64x64.pkt`, `msvideo1_64x64.ref` | msvideo1 | avi | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 5 -c:v msvideo1 -pix_fmt rgb555le` | --md5 |
| `qtrle_rgb24_64x64.pkt`, `qtrle_rgb24_64x64.ref` | qtrle | mov | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 5 -c:v qtrle -pix_fmt rgb24` | --md5 |
| `qtrle_argb_64x64.pkt`, `qtrle_argb_64x64.ref` | qtrle | mov | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 3 -c:v qtrle -pix_fmt argb` | --md5 |
| `qtrle_rgb555_64x64.pkt`, `qtrle_rgb555_64x64.ref` | qtrle | mov | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 3 -c:v qtrle -pix_fmt rgb555be` | --md5 |
| `qtrle_gray_64x64.pkt`, `qtrle_gray_64x64.ref` | qtrle | mov | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 3 -c:v qtrle -pix_fmt gray` | --md5 |
| `rpza_64x64.pkt`, `rpza_64x64.ref` | rpza | mov | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 4 -c:v rpza -pix_fmt rgb555le` | --md5 |
| `smc_64x64.pkt`, `smc_64x64.ref` | smc | mov | rgba | 64x64 | `-f lavfi -i testsrc2=s=64x64:r=25 -frames:v 4 -c:v smc -pix_fmt pal8` | --mov-palette --md5 |
| `raw_uyvy_64x32.pkt`, `raw_uyvy_64x32.ref` | uyvy | avi | yuv422p | 64x32 | `-f lavfi -i testsrc2=s=64x32:r=25 -frames:v 2 -c:v rawvideo -pix_fmt uyvy422` | --md5 |
| `raw_yuy2_64x32.pkt`, `raw_yuy2_64x32.ref` | yuy2 | avi | yuv422p | 64x32 | `-f lavfi -i testsrc2=s=64x32:r=25 -frames:v 2 -c:v rawvideo -pix_fmt yuyv422` | --md5 |
| `raw_bgr24_64x32.pkt`, `raw_bgr24_64x32.ref` | bgr24 | avi | rgba | 64x32 | `-f lavfi -i testsrc2=s=64x32:r=25 -frames:v 2 -c:v rawvideo -pix_fmt bgr24` | --md5 |
| `raw_v210_48x32.pkt`, `raw_v210_48x32.ref` | v210 | mov | yuv422p10le | 48x32 | `-f lavfi -i testsrc2=s=48x32:r=25 -frames:v 2 -c:v v210` | --md5 |
| `raw_r210_64x32.pkt`, `raw_r210_64x32.ref` | r210 | mov | rgba | 64x32 | `-f lavfi -i testsrc2=s=64x32:r=25 -frames:v 2 -c:v r210` | - |

## Interop of our encoders (development checks, not part of the test run)

With `CVT_LEGACY_DUMP=<dir>` the encoder tests write their streams (`<name>.pkt`) and sources
(`<name>.yuv`). Each stream was wrapped with `python3 tools/fixture_tool.py mkv <name>.pkt
<name>.mkv <codec spec>` and decoded by ffmpeg 6.1.1 (`ffmpeg -v error -i <name>.mkv -f null -`,
then the `psnr` filter against the source). Results (no decoder errors in any case):

| stream | Matroska codec spec | ffmpeg frames | PSNR vs source (average) |
| --- | --- | --- | --- |
| mpeg2video q95 / q90 / q70 / q40 (96x64, B-pictures) | `V_MPEG2` | 14 | 49.4 / 43.0 / 40.7 / 38.4 dB |
| mpeg1video q90 / q60 | `V_MPEG1` | 14 | 43.2 / 40.4 dB |
| mpeg4 q95 / q90 / q60 / q30 (96x64) | `V_MPEG4/ISO/ASP` | 12 | 50.4 / 43.7 / 40.6 / 35.9 dB |
| h263 q50 (128x96 baseline), q90 (96x72, PLUSPTYPE custom size) | `V_MS/VFW/FOURCC:H263` | 12 | 39.9 / 41.8 dB |
| flv1 q90 / q50 (72x40) | `V_MS/VFW/FOURCC:FLV1` | 12 | 43.6 / 38.9 dB |
| theora q95 / q80 / q50 / q20 (70x46) | `V_THEORA` | 10 | 43.1 / 41.1 / 39.2 / 33.2 dB |
| prores q100 / q90 / q60 / q30 (70x34, 4:2:2 10-bit) | `V_PRORES:apch` | 3 | 77.9 / 65.3 / 57.3 / 49.5 dB |
| prores 4444 with alpha (40x24) | `V_PRORES:ap4h` | 2 | decodes without errors |
| qtrle 24-bit and 32-bit (61x37, delta frames) | `V_QUICKTIME:rle:24`, `:32` | 6 | lossless (inf) |
| huffyuv 4:2:2 (64x40) and RGBA (33x17) | `V_MS/VFW/FOURCC:HFYU:16`, `:32` | 3 | lossless (inf) |
| ffv1 4:2:0, 4:2:2, 4:4:4, 4:2:0 10-bit, 4:2:2 10-bit, RGBA | `V_MS/VFW/FOURCC:FFV1` | 3 | lossless (inf) |

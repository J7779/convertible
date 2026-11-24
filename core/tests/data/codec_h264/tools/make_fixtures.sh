#!/bin/sh
# Regenerates the x264 fixtures of this directory. Run it inside a throwaway
# Ubuntu 24.04 container with ffmpeg 6.1.1 (libx264 core 164) and python3
# installed, with this directory as the working directory:
#   docker run --rm -v "$PWD:/w" -w /w ubuntu:24.04 sh -c \
#     'apt-get update && apt-get install -y ffmpeg python3 && sh tools/make_fixtures.sh'
# The VideoToolbox fixture is made on macOS; see SOURCES.md.
set -e
SRC="testsrc2=size=176x144:rate=25"
O="-map_metadata -1 -fflags +bitexact -flags:v +bitexact"
NOSEI="-bsf:v filter_units=remove_types=6"   # drop SEI (x264 writes its settings there)
enc() { # name frames source x264-options...
  name=$1; frames=$2; src=$3; shift 3
  ffmpeg -v error -y -f lavfi -i "$src" -frames:v "$frames" -c:v libx264 "$@" $NOSEI $O -f mp4 -movflags +faststart "$name.mp4"
  ffmpeg -v error -y -i "$name.mp4" -c:v copy -bsf:v h264_mp4toannexb $O -f h264 "$name.h264"
  python3 -I tools/mp4_to_pkt.py "$name.mp4" "$name.pkt"
  ffmpeg -v error -y -i "$name.h264" -f framemd5 "$name.md5"
  rm -f "$name.mp4"
}
enc baseline_cavlc_176x144 12 "$SRC" -profile:v baseline -pix_fmt yuv420p -crf 22 -g 6 -x264-params "ref=1"
enc main_cabac_bframes_176x144 16 "$SRC" -profile:v main -pix_fmt yuv420p -crf 22 -g 16 -bf 3 -x264-params "b-pyramid=normal:ref=3:b-adapt=0"
enc high_8x8_weighted_multiref_176x144 16 "$SRC" -profile:v high -pix_fmt yuv420p -crf 20 -g 16 -x264-params "8x8dct=1:weightp=2:weightb=1:ref=4:bframes=2:b-pyramid=none:b-adapt=0:direct=temporal"
enc high_cavlc_4slices_176x144 10 "$SRC" -profile:v high -pix_fmt yuv420p -crf 22 -g 10 -x264-params "cabac=0:slices=4:bframes=1:ref=2"
enc mbaff_176x144 10 "$SRC" -profile:v high -pix_fmt yuv420p -crf 22 -g 10 -flags +ildct+ilme -x264-params "tff=1:bframes=1:weightb=0"
enc mbaff_implicit_weights_176x144 10 "$SRC" -profile:v high -pix_fmt yuv420p -crf 22 -g 10 -flags +ildct+ilme -x264-params "tff=1:bframes=1:weightb=1"
enc crop_98x58_fullrange_bt709 10 "testsrc2=size=98x58:rate=30" -profile:v high -pix_fmt yuv420p -crf 22 -g 10 -x264-params "fullrange=on:colorprim=bt709:transfer=bt709:colormatrix=bt709:bframes=2"
enc crop_70x34_bt601 8 "testsrc2=size=70x34:rate=24" -profile:v main -pix_fmt yuv420p -crf 22 -x264-params "colorprim=smpte170m:transfer=smpte170m:colormatrix=smpte170m:bframes=0:sar=4/3"
enc unsupported_high422_32x32 2 "testsrc2=size=32x32:rate=25" -pix_fmt yuv422p -profile:v high422 -crf 30
enc unsupported_high10_32x32 2 "testsrc2=size=32x32:rate=25" -pix_fmt yuv420p10le -profile:v high10 -crf 30
# Only the files the tests use are kept.
rm -f baseline_cavlc_176x144.pkt high_cavlc_4slices_176x144.pkt crop_70x34_bt601.pkt \
      mbaff_implicit_weights_176x144.pkt unsupported_*.pkt unsupported_*.md5
# ffmpeg's decoding of the two B pictures of the implicit-weights MBAFF stream.
ffmpeg -v error -y -i mbaff_implicit_weights_176x144.h264 -f rawvideo /tmp/ref.yuv
python3 -I -c "
import zlib
d = open('/tmp/ref.yuv', 'rb').read()
f = 176 * 144 * 3 // 2
open('mbaff_implicit_weights_176x144.frames_1_3.yuv.zlib', 'wb').write(zlib.compress(d[f:2*f] + d[3*f:4*f], 9))
"

#!/bin/sh
# Regenerates the codec_legacy fixtures. Run from core/tests/data/codec_legacy:
#
#   docker run --rm --memory 2g -v "$PWD":/w -w /w <ubuntu 24.04 image with ffmpeg 6.1.1 and python3> sh tools/make_fixtures.sh
#
# Every clip is synthetic (lavfi testsrc2), encoded with -bitexact and without
# metadata, then dumped packet by packet with tools/fixture_tool.py; ffmpeg's
# own decode of the same file is stored as the reference (*.ref): pixels for
# the DCT codecs (selected frames), MD5 per frame for the bit-exact ones.
#
#   make NAME CODEC_ID EXT REF_PIX_FMT W H "ffmpeg input+encode args" "pkt options" "ref options"
set -eu
mkdir -p tmp
FF="ffmpeg -hide_banner -loglevel error -y"
BX="-map_metadata -1 -fflags +bitexact -flags:v +bitexact"

make() {
  name=$1 codec=$2 ext=$3 ref=$4 w=$5 h=$6 args=$7 pktopt=${8:-} refopt=${9:-}
  # ONLY="a b" regenerates just those fixtures.
  if [ -n "${ONLY:-}" ] && ! echo " $ONLY " | grep -q " $name "; then return 0; fi
  # shellcheck disable=SC2086
  $FF $args $BX tmp/$name.$ext
  # shellcheck disable=SC2086
  python3 tools/fixture_tool.py pkt tmp/$name.$ext $name.pkt $codec $pktopt
  $FF -i tmp/$name.$ext -fps_mode passthrough -f rawvideo -pix_fmt $ref tmp/$name.raw
  # shellcheck disable=SC2086
  python3 tools/fixture_tool.py ref tmp/$name.raw $name.ref $ref $w $h $refopt
}

src() { echo "-f lavfi -i testsrc2=s=$1:r=25 -frames:v $2"; }

# ---- MPEG-1 / MPEG-2 ---------------------------------------------------------
make mpeg1_96x64 mpeg1video mkv yuv420p 96 64 "$(src 96x64 12) -c:v mpeg1video -q:v 5 -bf 2 -g 9" "" "--frames=0,1,5,11"
make mpeg2_prog_96x64 mpeg2video mkv yuv420p 96 64 "$(src 96x64 12) -c:v mpeg2video -q:v 5 -bf 2 -g 9" "" "--frames=0,2,4,11"
make mpeg2_interlaced_96x64 mpeg2video mkv yuv420p 96 64 \
  "$(src 96x64 12) -c:v mpeg2video -q:v 5 -qmax 28 -bf 2 -g 9 -flags +ildct+ilme -top 1 -alternate_scan 1 -intra_vlc 1 -non_linear_quant 1 -dc 10" \
  "" "--frames=0,1,6,11"
make mpeg2_422_matrix_64x64 mpeg2video mkv yuv422p 64 64 \
  "$(src 64x64 8) -c:v mpeg2video -pix_fmt yuv422p -q:v 4 -bf 1 -g 6 -intra_matrix 8,16,19,22,26,27,29,34,16,16,22,24,27,29,34,37,19,22,26,27,29,34,34,38,22,22,26,27,29,34,37,40,22,26,27,29,32,35,40,48,26,27,29,32,35,40,48,58,26,27,29,34,38,46,56,69,27,29,35,38,46,56,69,83 -inter_matrix 20,17,18,19,20,21,22,23,17,18,19,20,21,22,23,24,18,19,20,21,22,23,24,25,19,20,21,22,23,24,26,27,20,21,22,23,25,26,27,28,21,22,23,24,26,27,28,30,22,23,24,26,27,28,30,31,23,24,25,27,28,30,31,33" \
  "" "--frames=0,1,7"
make mpeg2_long_gop_48x48 mpeg2video mkv yuv420p 48 48 \
  "$(src 48x48 120) -c:v mpeg2video -q:v 3 -bf 0 -g 600 -sc_threshold 1000000000" "" "--frames=0,119"

# ---- MPEG-4 Part 2 / H.263 family ----------------------------------------------
make mpeg4_asp_packed_96x64 mpeg4 avi yuv420p 96 64 \
  "$(src 96x64 12) -c:v mpeg4 -vtag XVID -q:v 5 -bf 2 -g 30 -flags +qpel+mv4" "--divx-packed=5" "--frames=0,2,6,11"
make mpeg4_global_header_96x64 mpeg4 mkv yuv420p 96 64 "$(src 96x64 10) -c:v mpeg4 -q:v 5 -bf 1 -g 12 -mpeg_quant 1" "" "--frames=0,1,9"
make mpeg4_xvid_gmc_96x64 mpeg4 avi yuv420p 96 64 "$(src 96x64 10) -c:v libxvid -q:v 5 -bf 2 -gmc 1 -flags +qpel" "" "--frames=0,1,5,9"
make mpeg4_interlaced_96x64 mpeg4 avi yuv420p 96 64 "$(src 96x64 8) -c:v mpeg4 -q:v 5 -bf 1 -flags +ildct+ilme" "" "--frames=0,1,7"
make mpeg4_sp_partitioned_96x64 mpeg4 avi yuv420p 96 64 "$(src 96x64 8) -c:v mpeg4 -q:v 5 -bf 0 -data_partitioning 1 -ps 120" "" "--frames=0,7"
make mpeg4_long_gop_48x48 mpeg4 avi yuv420p 48 48 "$(src 48x48 120) -c:v mpeg4 -q:v 3 -bf 0 -g 600 -sc_threshold 1000000000" "" "--frames=0,119"
make h263_128x96 h263 3gp yuv420p 128 96 "$(src 128x96 8) -c:v h263 -q:v 5 -g 12" "" "--frames=0,7"
make h263p_umv_160x120 h263 avi yuv420p 160 120 "$(src 160x120 8) -c:v h263p -q:v 5 -umv 1 -structured_slices 0" "" "--frames=0,7"
make flv1_64x64 flv1 flv yuv420p 64 64 "$(src 64x64 10) -c:v flv -q:v 5 -g 12" "" "--frames=0,9"

# ---- Theora (bit-exact by specification) ----------------------------------------
make theora_64x64 theora mkv yuv420p 64 64 "$(src 64x64 12) -c:v libtheora -q:v 6 -g 8" "--xiph" "--md5"

# ---- ProRes --------------------------------------------------------------------
for p in 0 1 2 3; do
  make prores_p${p}_64x64 prores mov yuv422p10le 64 64 "$(src 64x64 2) -c:v prores_ks -profile:v $p -pix_fmt yuv422p10le" "" "--frames=1"
done
make prores_4444_alpha_64x64 prores mov yuva444p10le 64 64 \
  "-f lavfi -i testsrc2=s=64x64:r=25,format=yuva444p10le,geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='16*X' -frames:v 2 -c:v prores_ks -profile:v 4 -pix_fmt yuva444p10le" \
  "" "--frames=1"
make prores_interlaced_64x64 prores mov yuv422p10le 64 64 "$(src 64x64 1) -c:v prores_ks -profile:v 3 -flags +ildct -pix_fmt yuv422p10le"

# ---- lossless (MD5 references) ------------------------------------------------------
make huffyuv_yuv422_left_64x64 huffyuv avi yuv422p 64 64 "$(src 64x64 3) -c:v huffyuv -pix_fmt yuv422p -pred left" "" "--md5"
make huffyuv_yuv422_median_64x64 huffyuv avi yuv422p 64 64 "$(src 64x64 3) -c:v huffyuv -pix_fmt yuv422p -pred median" "" "--md5"
make huffyuv_rgb24_64x64 huffyuv avi rgba 64 64 "$(src 64x64 3) -c:v huffyuv -pix_fmt rgb24 -pred left" "" "--md5"
make huffyuv_bgra_64x64 huffyuv avi rgba 64 64 "$(src 64x64 3) -c:v huffyuv -pix_fmt bgra -pred plane" "" "--md5"
make ffv1_v1_yuv420_64x64 ffv1 avi yuv420p 64 64 "$(src 64x64 3) -c:v ffv1 -level 1 -coder 0 -pix_fmt yuv420p" "" "--md5"
make ffv1_v3_yuv420_slices_64x64 ffv1 mkv yuv420p 64 64 \
  "$(src 64x64 3) -c:v ffv1 -level 3 -coder 1 -context 1 -slices 4 -slicecrc 1 -pix_fmt yuv420p" "" "--md5"
make ffv1_v3_rgb_64x64 ffv1 mkv rgba 64 64 "$(src 64x64 2) -c:v ffv1 -level 3 -coder 1 -pix_fmt bgr0" "" "--md5"
make ffv1_v3_yuv422p10_64x64 ffv1 mkv yuv422p10le 64 64 "$(src 64x64 2) -c:v ffv1 -level 3 -coder 2 -pix_fmt yuv422p10le" "" "--md5"
make utvideo_yuv420_64x64 utvideo avi yuv420p 64 64 "$(src 64x64 2) -c:v utvideo -pix_fmt yuv420p -pred left" "" "--md5"
make utvideo_yuv422_median_64x64 utvideo avi yuv422p 64 64 "$(src 64x64 2) -c:v utvideo -pix_fmt yuv422p -pred median" "" "--md5"
make utvideo_rgb_median_64x64 utvideo avi rgba 64 64 "$(src 64x64 2) -c:v utvideo -pix_fmt gbrp -pred median" "" "--md5"
make utvideo_rgba_64x64 utvideo avi rgba 64 64 "$(src 64x64 2) -c:v utvideo -pix_fmt gbrap -pred left" "" "--md5"

# ---- simple RGB / palette codecs (MD5 references) -------------------------------------
make cinepak_64x64 cinepak avi rgba 64 64 "$(src 64x64 5) -c:v cinepak -pix_fmt rgb24" "" "--md5"
make msrle_64x64 msrle avi rgba 64 64 "$(src 64x64 5) -c:v msrle -pix_fmt pal8" "" "--md5"
make msvideo1_64x64 msvideo1 avi rgba 64 64 "$(src 64x64 5) -c:v msvideo1 -pix_fmt rgb555le" "" "--md5"
make qtrle_rgb24_64x64 qtrle mov rgba 64 64 "$(src 64x64 5) -c:v qtrle -pix_fmt rgb24" "" "--md5"
make qtrle_argb_64x64 qtrle mov rgba 64 64 "$(src 64x64 3) -c:v qtrle -pix_fmt argb" "" "--md5"
make qtrle_rgb555_64x64 qtrle mov rgba 64 64 "$(src 64x64 3) -c:v qtrle -pix_fmt rgb555be" "" "--md5"
make qtrle_gray_64x64 qtrle mov rgba 64 64 "$(src 64x64 3) -c:v qtrle -pix_fmt gray" "" "--md5"
make rpza_64x64 rpza mov rgba 64 64 "$(src 64x64 4) -c:v rpza -pix_fmt rgb555le" "" "--md5"
make smc_64x64 smc mov rgba 64 64 "$(src 64x64 4) -c:v smc -pix_fmt pal8" "--mov-palette" "--md5"

# ---- uncompressed variants ---------------------------------------------------------
make raw_uyvy_64x32 uyvy avi yuv422p 64 32 "$(src 64x32 2) -c:v rawvideo -pix_fmt uyvy422" "" "--md5"
make raw_yuy2_64x32 yuy2 avi yuv422p 64 32 "$(src 64x32 2) -c:v rawvideo -pix_fmt yuyv422" "" "--md5"
make raw_bgr24_64x32 bgr24 avi rgba 64 32 "$(src 64x32 2) -c:v rawvideo -pix_fmt bgr24" "" "--md5"
make raw_v210_48x32 v210 mov yuv422p10le 48 32 "$(src 48x32 2) -c:v v210" "" "--md5"
make raw_r210_64x32 r210 mov rgba 64 32 "$(src 64x32 2) -c:v r210"

[ "${KEEP_TMP:-0}" = 1 ] || rm -rf tmp

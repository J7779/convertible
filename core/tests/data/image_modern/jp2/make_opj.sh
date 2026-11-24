#!/bin/sh
# Encodes the OpenJPEG-made fixtures and their reference decodes. Run inside a
# throwaway Debian trixie container with this directory mounted at /w:
#   docker run --rm --memory 2g -v "$PWD":/w -w /w debian:trixie sh make_opj.sh
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq libopenjp2-tools >/dev/null
opj_compress -h 2>/dev/null | head -n 3 | tail -n 1 || true

opj_compress -i src_rgb.ppm -o rgb_lossless.jp2
opj_compress -i src_rgb.ppm -o rgb_lossy97.jp2 -I -r 12
opj_decompress -i rgb_lossy97.jp2 -o rgb_lossy97.ref.ppm
opj_compress -i src_rgb.ppm -o tiles_layers_rpcl.j2k -t 16,16 -n 3 -r 30,8,1 -p RPCL
opj_compress -i src_rgb.ppm -o cprl_layers97.jp2 -I -q 28,36,44 -p CPRL -n 4 -b 16,16
opj_decompress -i cprl_layers97.jp2 -o cprl_layers97.ref.ppm
opj_compress -i src_gray16.pgm -o gray16.j2k
opj_compress -i src_s12_32x24.raw -o s12_signed.j2k -F 32,24,1,12,s -n 5
opj_compress -i src_ycc420_48x40.raw -o ycc420.j2k -F 48,40,3,8,u@1x1:2x2:2x2 -n 5
opj_decompress -i ycc420.j2k -o ycc420.ref.ppm
opj_compress -i src_rgba.png -o rgba_cdef.jp2 -n 5
opj_compress -i src_index.pgm -o index.j2k -n 5

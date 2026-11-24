#!/bin/sh
# Run inside a throwaway ubuntu:24.04 container with this folder mounted at /w
# (after make_fixtures.py): builds the YCA writer and makes the mipmap and
# ripmap fixtures with OpenEXR's exrmaketiled.
set -e
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
  openexr libopenexr-dev g++ pkg-config > /dev/null
cd /w
g++ -O1 make_yca.cc $(pkg-config --cflags --libs OpenEXR) -o /tmp/make_yca
/tmp/make_yca
exrmaketiled -m -d -t 16 8 -z piz rgb_half_zip.exr mipmap_piz.exr
exrmaketiled -r -u -t 8 8 -z rle rgb_half_zip.exr ripmap_rle.exr
for f in luma_chroma_piz.exr mipmap_piz.exr ripmap_rle.exr; do exrheader "$f"; done

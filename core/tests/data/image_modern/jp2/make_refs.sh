#!/bin/sh
# Reference decodes of the wrapped files (after make_wrapped.py), in the same
# kind of throwaway container as make_opj.sh:
#   docker run --rm --memory 2g -v "$PWD":/w -w /w debian:trixie sh make_refs.sh
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq libopenjp2-tools >/dev/null
opj_decompress -i palette.jp2 -o palette.ref.ppm
opj_decompress -i sycc420.jp2 -o sycc420.ref.ppm
opj_decompress -i rgb.jpx -o rgb_jpx.ref.ppm

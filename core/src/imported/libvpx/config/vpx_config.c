/* Copyright (c) 2011 The WebM project authors. All Rights Reserved. */
/*  */
/* Use of this source code is governed by a BSD-style license */
/* that can be found in the LICENSE file in the root of the source */
/* tree. An additional intellectual property rights grant can be found */
/* in the file PATENTS.  All contributing project authors may */
/* be found in the AUTHORS file in the root of the source tree. */
#include "vpx/vpx_codec.h"
static const char* const cfg = "--target=generic-gnu --disable-multithread --enable-vp9-highbitdepth --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-webm-io --disable-libyuv --enable-static --disable-shared --disable-postproc --disable-vp9-postproc --disable-runtime-cpu-detect --enable-vp8 --enable-vp9 --disable-debug --size-limit=16384x16384";
const char *vpx_codec_build_config(void) {return cfg;}

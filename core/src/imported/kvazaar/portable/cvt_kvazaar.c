/*
 * Narrow C interface to the Kvazaar encoder (see api/cvt_kvazaar.h).
 * Written for Convertible; BSD-3-Clause like the code it wraps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvazaar.h"
#include "strategyselector.h"

#include "cvt_kvazaar.h"

struct cvt_kvazaar_encoder {
  const kvz_api *api;
  kvz_config *config;
  kvz_encoder *encoder;
  int32_t width;          /* configured (real) size */
  int32_t height;
  int32_t padded_width;   /* picture size: multiple of 8 */
  int32_t padded_height;
};

int cvt_kvazaar_global_init(void) {
  /* cpuid = 0: no CPU probing; the C-only build has only generic strategies. */
  return kvz_strategyselector_init(0, KVZ_BIT_DEPTH, 0) ? 1 : 0;
}

static int set_int(const kvz_api *api, kvz_config *config, const char *name, int32_t value) {
  char text[24];
  snprintf(text, sizeof(text), "%d", (int)value);
  return api->config_parse(config, name, text);
}

static int set_text(const kvz_api *api, kvz_config *config, const char *name, const char *value) {
  return api->config_parse(config, name, value);
}

static int32_t padded(int32_t size) { return (size + 7) & ~7; }

cvt_kvazaar_encoder *cvt_kvazaar_open(const cvt_kvazaar_params *p) {
  cvt_kvazaar_encoder *enc;
  const kvz_api *api = kvz_api_get(8);
  int ok = 1;
  char fps[48];
  if(!api || !p) return NULL;
  if(p->width <= 0 || p->height <= 0 || (p->width & 1) || (p->height & 1)) return NULL;

  enc = (cvt_kvazaar_encoder *)calloc(1, sizeof(*enc));
  if(!enc) return NULL;
  enc->api = api;
  enc->width = p->width;
  enc->height = p->height;
  enc->padded_width = padded(p->width);
  enc->padded_height = padded(p->height);
  enc->config = api->config_alloc();
  if(!enc->config || !api->config_init(enc->config)) {
    cvt_kvazaar_close(enc);
    return NULL;
  }

  /* Nothing is written to stderr; then the preset, which later options override. */
  ok &= set_int(api, enc->config, "enable-logging", 0);
  ok &= set_int(api, enc->config, "preset", p->preset < 0 ? 0 : (p->preset > 9 ? 9 : p->preset));
  ok &= set_int(api, enc->config, "width", p->width);
  ok &= set_int(api, enc->config, "height", p->height);
  snprintf(fps, sizeof(fps), "%d/%d", (int)(p->fps_num > 0 ? p->fps_num : 25), (int)(p->fps_den > 0 ? p->fps_den : 1));
  ok &= set_text(api, enc->config, "input-fps", fps);
  /* Single-threaded, deterministic, no CPU probing. */
  ok &= set_int(api, enc->config, "threads", 0);
  ok &= set_int(api, enc->config, "owf", 0);
  ok &= set_int(api, enc->config, "cpuid", 0);
  ok &= set_int(api, enc->config, "info", 0);      /* no encoder-information SEI */
  ok &= set_text(api, enc->config, "hash", "none");
  ok &= set_int(api, enc->config, "wpp", 0);
  ok &= set_int(api, enc->config, "aud", 0);
  ok &= set_int(api, enc->config, "vps-period", 0);
  ok &= set_int(api, enc->config, "open-gop", 0);   /* closed GOPs: every keyframe is an IDR */
  if(p->gop == 8) {
    ok &= set_text(api, enc->config, "gop", "8");
    /* Hierarchical B pictures need references on both sides; the fast
     * presets allow only one. */
    if(enc->config->ref_frames < 4) ok &= set_int(api, enc->config, "ref", 4);
  } else {
    ok &= set_text(api, enc->config, "gop", "lp-g4d3t1");
  }
  ok &= set_int(api, enc->config, "period", p->intra_period < 0 ? 0 : p->intra_period);
  if(p->bitrate > 0) {
    ok &= set_int(api, enc->config, "bitrate", p->bitrate);
  } else {
    ok &= set_int(api, enc->config, "qp", p->qp < 0 ? 0 : (p->qp > 51 ? 51 : p->qp));
  }
  /* VUI colour description, as ITU-T H.273 code points. */
  enc->config->vui.colorprim = (int8_t)p->colorprim;
  enc->config->vui.transfer = (int8_t)p->transfer;
  enc->config->vui.colormatrix = (int8_t)p->colormatrix;
  enc->config->vui.fullrange = (int8_t)(p->fullrange ? 1 : 0);
  enc->config->vui.chroma_loc = (int8_t)p->chroma_loc;
  if(!ok) {
    cvt_kvazaar_close(enc);
    return NULL;
  }
  enc->encoder = api->encoder_open(enc->config);
  if(!enc->encoder) {
    cvt_kvazaar_close(enc);
    return NULL;
  }
  return enc;
}

void cvt_kvazaar_close(cvt_kvazaar_encoder *enc) {
  if(!enc) return;
  if(enc->encoder) enc->api->encoder_close(enc->encoder);
  if(enc->config) enc->api->config_destroy(enc->config);
  free(enc);
}

static uint8_t *take_chunks(const kvz_api *api, kvz_data_chunk *chunks, uint32_t length, size_t *size) {
  uint8_t *data;
  size_t at = 0;
  kvz_data_chunk *chunk;
  *size = 0;
  data = (uint8_t *)malloc(length > 0 ? length : 1);
  if(!data) {
    api->chunk_free(chunks);
    return NULL;
  }
  for(chunk = chunks; chunk != NULL; chunk = chunk->next) {
    if(at + chunk->len > length) break;
    memcpy(data + at, chunk->data, chunk->len);
    at += chunk->len;
  }
  api->chunk_free(chunks);
  *size = at;
  return data;
}

int cvt_kvazaar_headers(cvt_kvazaar_encoder *enc, uint8_t **data, size_t *size) {
  kvz_data_chunk *chunks = NULL;
  uint32_t length = 0;
  *data = NULL;
  *size = 0;
  if(!enc->api->encoder_headers(enc->encoder, &chunks, &length)) return 0;
  *data = take_chunks(enc->api, chunks, length, size);
  return *data != NULL;
}

/* Copy one plane into a picture plane, replicating the last column and row
 * into the padding. */
static void copy_plane(kvz_pixel *dst, int32_t dst_stride, int32_t dst_w, int32_t dst_h, const uint8_t *src,
                       int32_t src_stride, int32_t src_w, int32_t src_h) {
  int32_t y;
  for(y = 0; y < dst_h; ++y) {
    const uint8_t *row = src + (size_t)(y < src_h ? y : src_h - 1) * (size_t)src_stride;
    kvz_pixel *out = dst + (size_t)y * (size_t)dst_stride;
    int32_t x;
    memcpy(out, row, (size_t)src_w);
    for(x = src_w; x < dst_w; ++x) out[x] = row[src_w - 1];
  }
}

int cvt_kvazaar_encode(cvt_kvazaar_encoder *enc, const uint8_t *const planes[3], const int32_t strides[3],
                       int64_t pts, cvt_kvazaar_packet *packet) {
  kvz_picture *input = NULL;
  kvz_data_chunk *chunks = NULL;
  uint32_t length = 0;
  kvz_picture *source = NULL;
  kvz_frame_info info;
  int ok;
  memset(packet, 0, sizeof(*packet));
  memset(&info, 0, sizeof(info));

  if(planes && planes[0]) {
    input = enc->api->picture_alloc_csp(KVZ_CSP_420, enc->padded_width, enc->padded_height);
    if(!input) return 0;
    copy_plane(input->y, input->stride, enc->padded_width, enc->padded_height, planes[0], strides[0], enc->width,
               enc->height);
    copy_plane(input->u, input->stride / 2, enc->padded_width / 2, enc->padded_height / 2, planes[1], strides[1],
               enc->width / 2, enc->height / 2);
    copy_plane(input->v, input->stride / 2, enc->padded_width / 2, enc->padded_height / 2, planes[2], strides[2],
               enc->width / 2, enc->height / 2);
    input->pts = pts;
  }

  ok = enc->api->encoder_encode(enc->encoder, input, &chunks, &length, NULL, &source, &info);
  if(input) enc->api->picture_free(input);
  if(!ok) {
    if(chunks) enc->api->chunk_free(chunks);
    if(source) enc->api->picture_free(source);
    return 0;
  }
  if(chunks) {
    packet->data = take_chunks(enc->api, chunks, length, &packet->size);
    if(!packet->data) {
      if(source) enc->api->picture_free(source);
      return 0;
    }
    packet->pts = source ? source->pts : 0;
    packet->dts = source ? source->dts : 0;
    packet->nal_unit_type = (int32_t)info.nal_unit_type;
    packet->slice_type = (int32_t)info.slice_type;
  }
  if(source) enc->api->picture_free(source);
  return 1;
}

void cvt_kvazaar_free(void *data) { free(data); }

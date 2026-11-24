/*
 * Narrow C interface to the Kvazaar encoder, used by Convertible's
 * codec_hevc family, so that Kvazaar's own headers never enter the core's
 * include path. Written for Convertible; BSD-3-Clause like the code it wraps.
 */
#ifndef CVT_KVAZAAR_H
#define CVT_KVAZAAR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prepare Kvazaar's process-wide function tables. Must complete once before
 * the first cvt_kvazaar_open (the C++ wrapper guards it with std::call_once);
 * the tables never change afterwards. Returns 1 on success. */
int cvt_kvazaar_global_init(void);

typedef struct cvt_kvazaar_encoder cvt_kvazaar_encoder;

typedef struct {
  int32_t width;            /* luma size; even, > 0 */
  int32_t height;
  int32_t fps_num;          /* nominal frame rate (used by rate control) */
  int32_t fps_den;
  int32_t qp;               /* 0..51, constant-QP mode when bitrate == 0 */
  int32_t bitrate;          /* target bits per second; 0 = constant QP */
  int32_t intra_period;     /* frames between IRAP pictures; 0 = first only */
  int32_t gop;              /* 0 = I/P only (low delay), 8 = hierarchical B */
  int32_t preset;           /* Kvazaar preset 0 (ultrafast) .. 9 (placebo) */
  int32_t colorprim;        /* VUI values (ITU-T H.273 code points) */
  int32_t transfer;
  int32_t colormatrix;
  int32_t fullrange;
  int32_t chroma_loc;
} cvt_kvazaar_params;

/* NULL when the parameters are rejected or memory is short. */
cvt_kvazaar_encoder *cvt_kvazaar_open(const cvt_kvazaar_params *params);
void cvt_kvazaar_close(cvt_kvazaar_encoder *encoder);

/* Annex-B VPS, SPS and PPS. *data is malloc'd; release with cvt_kvazaar_free. */
int cvt_kvazaar_headers(cvt_kvazaar_encoder *encoder, uint8_t **data, size_t *size);

typedef struct {
  uint8_t *data;      /* Annex-B access unit, malloc'd; NULL when none */
  size_t size;
  int64_t pts;        /* the `pts` given with the source picture */
  int64_t dts;
  int32_t nal_unit_type;
  int32_t slice_type; /* 0 B, 1 P, 2 I */
} cvt_kvazaar_packet;

/* Feed one 4:2:0 picture (planes of width x height and (width/2) x (height/2)
 * samples with the given strides), or NULL planes to drain. At most one
 * packet is returned per call. Returns 1 on success, 0 on failure. */
int cvt_kvazaar_encode(cvt_kvazaar_encoder *encoder, const uint8_t *const planes[3], const int32_t strides[3],
                       int64_t pts, cvt_kvazaar_packet *packet);

void cvt_kvazaar_free(void *data);

#ifdef __cplusplus
}
#endif

#endif /* CVT_KVAZAAR_H */

/*
 * Narrow C interface to the libhevc decoder, used by Convertible's codec_hevc
 * family. It is the only libhevc header visible outside the library, so the
 * decoder's own headers (iv.h, ivd.h, ...) never enter the core's include
 * path. Written for Convertible; Apache-2.0 like the code it wraps.
 */
#ifndef CVT_LIBHEVC_H
#define CVT_LIBHEVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cvt_libhevc_decoder cvt_libhevc_decoder;

/* Output layout. The decoder writes the cropped (display) picture, tightly
 * packed: luma rows of width samples; chroma rows of (width + 1) / 2 samples
 * for 4:2:0 and 4:2:2, width samples for 4:4:4. Samples are bytes for 8-bit
 * streams and native-endian 16-bit words for 10-bit streams. Monochrome
 * (4:0:0) streams are delivered as 4:2:0 with neutral chroma. */
enum {
  CVT_LIBHEVC_OUT_420 = 1,
  CVT_LIBHEVC_OUT_422 = 2,
  CVT_LIBHEVC_OUT_444 = 3
};

/* Create a single-threaded decoder producing `out_layout` pictures.
 * Returns NULL when out of memory. */
cvt_libhevc_decoder *cvt_libhevc_create(int out_layout);
void cvt_libhevc_destroy(cvt_libhevc_decoder *decoder);

typedef struct {
  /* in: destination planes (Y, Cb, Cr) and their sizes in bytes */
  uint8_t *planes[3];
  uint32_t plane_sizes[3];
  /* out */
  uint32_t bytes_consumed;
  int picture_decoded; /* a picture was decoded from this input */
  int output_present;  /* a picture (in display order) was written to planes */
  uint32_t output_timestamp; /* the timestamp passed with that picture's data */
  uint32_t width;       /* display size of the stream, 0 until known */
  uint32_t height;
  uint32_t bit_depth;   /* 8 or 10, 0 until known */
  uint32_t error_code;  /* libhevc error code of this call, 0 when none */
  int fatal;            /* the error is fatal: the stream cannot be decoded further */
} cvt_libhevc_io;

/* Decode from Annex-B data (start codes required). One call decodes at most
 * one picture and outputs at most one picture; call again with the remaining
 * bytes. `size` == 0 with flush() before drains the pictures still held for
 * reordering. Returns 0 on success, -1 on failure (see io->error_code). */
int cvt_libhevc_decode(cvt_libhevc_decoder *decoder, const uint8_t *data, uint32_t size,
                       uint32_t timestamp, cvt_libhevc_io *io);

/* Switch to flush mode: subsequent decode calls (with size 0) return the
 * remaining pictures, one per call, until output_present is 0. */
int cvt_libhevc_flush(cvt_libhevc_decoder *decoder);

/* Back to normal decoding after a flush (for instance after a seek). */
int cvt_libhevc_reset(cvt_libhevc_decoder *decoder);

#ifdef __cplusplus
}
#endif

#endif /* CVT_LIBHEVC_H */

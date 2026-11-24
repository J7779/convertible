/* Convertible: stand-in for the Brotli encoder, which libjxl only needs to
 * write Brotli-compressed metadata boxes ("brob") and JPEG reconstruction
 * data. Convertible never requests either (boxes are always stored plain and
 * JPEG recompression is not offered), so the encoder (about 670 KiB of code
 * and tables) is left out and creating an instance simply fails, which libjxl
 * reports as an ordinary encoding error. The symbols are renamed by
 * cvt_jxl_prefix.h like the rest of the private brotli copy. */
#include <stddef.h>
#include <stdint.h>

#include <brotli/encode.h>

BrotliEncoderState* BrotliEncoderCreateInstance(brotli_alloc_func alloc_func, brotli_free_func free_func,
                                                void* opaque) {
  (void)alloc_func;
  (void)free_func;
  (void)opaque;
  return NULL;
}

void BrotliEncoderDestroyInstance(BrotliEncoderState* state) { (void)state; }

BROTLI_BOOL BrotliEncoderSetParameter(BrotliEncoderState* state, BrotliEncoderParameter param, uint32_t value) {
  (void)state;
  (void)param;
  (void)value;
  return BROTLI_FALSE;
}

BROTLI_BOOL BrotliEncoderCompressStream(BrotliEncoderState* state, BrotliEncoderOperation op, size_t* available_in,
                                        const uint8_t** next_in, size_t* available_out, uint8_t** next_out,
                                        size_t* total_out) {
  (void)state;
  (void)op;
  (void)available_in;
  (void)next_in;
  (void)available_out;
  (void)next_out;
  (void)total_out;
  return BROTLI_FALSE;
}

BROTLI_BOOL BrotliEncoderIsFinished(BrotliEncoderState* state) {
  (void)state;
  return BROTLI_TRUE;
}

BROTLI_BOOL BrotliEncoderHasMoreOutput(BrotliEncoderState* state) {
  (void)state;
  return BROTLI_FALSE;
}

size_t BrotliEncoderMaxCompressedSize(size_t input_size) {
  /* Upper bound used by upstream for incompressible input. */
  return input_size + (input_size >> 2) + 1024;
}

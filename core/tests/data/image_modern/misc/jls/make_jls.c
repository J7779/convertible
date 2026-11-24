/* Writes JPEG-LS fixtures with CharLS (the reference JPEG-LS implementation)
 * and, for each, CharLS's own decoding of it as a PAM file.
 *   cc -O2 make_jls.c -lcharls -lm -o make_jls && ./make_jls
 * The pictures are synthetic: gradients, a sine pattern, flat areas (runs),
 * hard edges and pseudo-random noise from a fixed linear congruential
 * generator. */
#include <charls/charls.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t seed = 12345;
static uint32_t lcg(void) {
  seed = seed * 1103515245u + 12345u;
  return (seed >> 16) & 0x7FFF;
}

static int sample(int x, int y, int c, int w, int h, int maxval) {
  if (x < w / 4 && y < h / 3) return c == 3 ? maxval : (maxval / 5) * (c + 1); /* flat block: runs */
  if (x > w * 3 / 4 && y > h / 2) return ((x / 4 + y / 4) % 2) ? maxval : 0;  /* checker: edges */
  double v = (double)x / (w - 1) * 0.6 + (double)y / (h - 1) * 0.3 + 0.1 * c;
  v += 0.08 * sin(x * 0.4 + c) * cos(y * 0.3);
  if (c == 3) v = 1.0 - (double)x / (w - 1) * 0.8;
  int s = (int)(v * maxval + 0.5) + (int)(lcg() % 7) - 3;
  if (s < 0) s = 0;
  if (s > maxval) s = maxval;
  return s;
}

static void write_pam(const char* path, const void* data, int w, int h, int comps, int maxval, int planar) {
  FILE* f = fopen(path, "wb");
  static const char* types[] = {"", "GRAYSCALE", "GRAYSCALE_ALPHA", "RGB", "RGB_ALPHA"};
  fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH %d\nMAXVAL %d\nTUPLTYPE %s\nENDHDR\n", w, h, comps, maxval, types[comps]);
  const int wide = maxval > 255;
  for (int i = 0; i < w * h; ++i) {
    for (int c = 0; c < comps; ++c) {
      const size_t index = planar ? (size_t)c * w * h + i : (size_t)i * comps + c;
      unsigned v = wide ? ((const uint16_t*)data)[index] : ((const uint8_t*)data)[index];
      if (wide) fputc(v >> 8, f);
      fputc(v & 0xFF, f);
    }
  }
  fclose(f);
}

static void make(const char* name, int w, int h, int bits, int comps, int maxval, charls_interleave_mode ilv, int near,
                 const charls_jpegls_pc_parameters* pc) {
  const int wide = bits > 8;
  const size_t bytes = (size_t)w * h * comps * (wide ? 2 : 1);
  void* src = malloc(bytes);
  const int planar = ilv == CHARLS_INTERLEAVE_MODE_NONE;
  seed = 12345;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < comps; ++c) {
        const size_t index = planar ? (size_t)c * w * h + (size_t)y * w + x : ((size_t)y * w + x) * comps + c;
        const int v = sample(x, y, c, w, h, maxval);
        if (wide) ((uint16_t*)src)[index] = (uint16_t)v;
        else ((uint8_t*)src)[index] = (uint8_t)v;
      }
  charls_jpegls_encoder* enc = charls_jpegls_encoder_create();
  charls_frame_info fi = {(uint32_t)w, (uint32_t)h, bits, comps};
  charls_jpegls_encoder_set_frame_info(enc, &fi);
  charls_jpegls_encoder_set_near_lossless(enc, near);
  if (comps > 1) charls_jpegls_encoder_set_interleave_mode(enc, ilv);
  if (pc) charls_jpegls_encoder_set_preset_coding_parameters(enc, pc);
  size_t cap = 0;
  charls_jpegls_encoder_get_estimated_destination_size(enc, &cap);
  void* dst = malloc(cap);
  charls_jpegls_encoder_set_destination_buffer(enc, dst, cap);
  int err = charls_jpegls_encoder_encode_from_buffer(enc, src, bytes, 0);
  if (err) {
    fprintf(stderr, "%s: encode error %d\n", name, err);
    exit(1);
  }
  size_t written = 0;
  charls_jpegls_encoder_get_bytes_written(enc, &written);
  char path[256];
  snprintf(path, sizeof path, "%s.jls", name);
  FILE* f = fopen(path, "wb");
  fwrite(dst, 1, written, f);
  fclose(f);
  /* CharLS's own decoding of the file. */
  charls_jpegls_decoder* dec = charls_jpegls_decoder_create();
  charls_jpegls_decoder_set_source_buffer(dec, dst, written);
  charls_jpegls_decoder_read_header(dec);
  void* out = malloc(bytes);
  err = charls_jpegls_decoder_decode_to_buffer(dec, out, bytes, 0);
  if (err) {
    fprintf(stderr, "%s: decode error %d\n", name, err);
    exit(1);
  }
  snprintf(path, sizeof path, "%s.pam", name);
  write_pam(path, out, w, h, comps, maxval, planar);
  if (near == 0 && memcmp(out, src, bytes) != 0) fprintf(stderr, "%s: lossless mismatch\n", name);
  printf("%s %zu bytes\n", name, written);
  charls_jpegls_decoder_destroy(dec);
  charls_jpegls_encoder_destroy(enc);
  free(src);
  free(dst);
  free(out);
}

int main(void) {
  const charls_interleave_mode none = CHARLS_INTERLEAVE_MODE_NONE, line = CHARLS_INTERLEAVE_MODE_LINE,
                               samp = CHARLS_INTERLEAVE_MODE_SAMPLE;
  make("grey8", 64, 48, 8, 1, 255, none, 0, NULL);
  make("grey8_near3", 64, 48, 8, 1, 255, none, 3, NULL);
  make("grey5", 40, 30, 5, 1, 31, none, 0, NULL);
  make("grey12", 40, 30, 12, 1, 4095, none, 0, NULL);
  make("grey16_near2", 40, 30, 16, 1, 65535, none, 2, NULL);
  make("rgb8_none", 48, 32, 8, 3, 255, none, 0, NULL);
  make("rgb8_line", 48, 32, 8, 3, 255, line, 0, NULL);
  make("rgb8_sample", 48, 32, 8, 3, 255, samp, 0, NULL);
  make("rgb8_line_near2", 48, 32, 8, 3, 255, line, 2, NULL);
  make("rgb8_sample_near4", 48, 32, 8, 3, 255, samp, 4, NULL);
  make("rgb16_sample", 33, 21, 16, 3, 65535, samp, 0, NULL);
  make("rgba8_sample", 40, 30, 8, 4, 255, samp, 0, NULL);
  const charls_jpegls_pc_parameters custom = {255, 5, 12, 40, 32};
  make("grey8_custom", 64, 48, 8, 1, 255, none, 0, &custom);
  const charls_jpegls_pc_parameters custom_near = {0, 9, 20, 50, 0};
  make("rgb8_custom_near1", 48, 32, 8, 3, 255, samp, 1, &custom_near);
  return 0;
}

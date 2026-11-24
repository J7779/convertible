// Writes luma_chroma_piz.exr: a 36x22 RGB test pattern stored by the
// reference library as luminance + 2x2-subsampled chroma (Y, RY, BY).
// Build: g++ make_yca.cc $(pkg-config --cflags --libs OpenEXR)
#include <ImfRgbaFile.h>
#include <ImfArray.h>

int main() {
  const int w = 36, h = 22;
  Imf::Array2D<Imf::Rgba> px(h, w);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Constant chroma ratio, smooth luminance.
      const float v = 0.5f + x / 8.0f + y / 16.0f;
      px[y][x] = Imf::Rgba(v * 1.25f, v, v * 0.75f, 1.0f);
    }
  }
  Imf::RgbaOutputFile file("luma_chroma_piz.exr", w, h, Imf::WRITE_YC, 1, Imath::V2f(0, 0), 1,
                           Imf::INCREASING_Y, Imf::PIZ_COMPRESSION);
  file.setFrameBuffer(&px[0][0], 1, w);
  file.writePixels(h);
  return 0;
}

// Convertible core - image family: registration entry points of each format.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::image {

void register_png(Registry& registry);       // png, apng + "png" video codec
void register_jpeg(Registry& registry);      // jpg (+ jfif/jpe aliases), mpo + "mjpeg" video codec
void register_gif(Registry& registry);
void register_bmp(Registry& registry);
void register_ico(Registry& registry);       // ico, cur
void register_icns(Registry& registry);
void register_tga(Registry& registry);
void register_pcx(Registry& registry);
void register_tiff(Registry& registry);
void register_psd(Registry& registry);       // psd, psb
void register_dds(Registry& registry);
void register_pnm(Registry& registry);       // pbm, pgm, ppm, pnm, pam, pfm
void register_qoi(Registry& registry);
void register_hdr(Registry& registry);       // Radiance RGBE
void register_wbmp(Registry& registry);
void register_xbm(Registry& registry);
void register_xpm(Registry& registry);
void register_farbfeld(Registry& registry);
void register_sgi(Registry& registry);
void register_sun(Registry& registry);

}  // namespace cvt::image

#!/bin/bash
# KTX fixtures with KTX-Software 4.4.2 (ktx create, toktx). Run in the texture
# fixture directory after make_textures.py.
set -e
ktx create --format R8G8B8A8_SRGB src_rgba.png rgba8_srgb.ktx2
ktx create --format R8G8B8A8_SRGB --zlib 9 src_rgba.png rgba8_zlib.ktx2
ktx create --format R8G8B8A8_SRGB --zstd 5 src_rgba.png rgba8_zstd.ktx2
ktx create --format R8G8B8A8_SRGB --encode basis-lz src_rgba.png basis.ktx2
ktx create --format R16G16B16A16_UNORM --assign-tf srgb src16.png rgba16.ktx2
ktx create --format R8_SRGB src_grey.png r8.ktx2
ktx create --format ASTC_8x8_UNORM_BLOCK src_rgba.png astc8x8.ktx2
for f in BC1_RGBA_UNORM:bc1 BC3_UNORM:bc3 BC4_UNORM:bc4 BC5_UNORM:bc5 BC7_UNORM:bc7 ETC2_R8G8B8_UNORM:etc1 \
         ETC2_R8G8B8_UNORM:etc2 ETC2_R8G8B8A8_UNORM:etc2a8 EAC_R11_UNORM:eacr EAC_R11G11_UNORM:eacrg; do
  ktx create --raw --width 64 --height 48 --format ${f%%:*}_BLOCK ${f##*:}.raw ${f##*:}.ktx2
done
toktx rgba8.ktx src_rgba.png
toktx rgb8_odd.ktx src_rgb_odd.png
toktx rgba16.ktx src16.png

// Convertible desktop - writing screenshots (PNG and BMP).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gfx/canvas.hpp"

namespace cvd::gfx {

/// Encodes an RGBA8 PNG (zlib with stored and fixed-Huffman RLE blocks; no external code).
std::vector<std::uint8_t> encode_png(const Bitmap& bitmap);
/// Encodes a 32-bit BMP (BGRA, top-down).
std::vector<std::uint8_t> encode_bmp(const Bitmap& bitmap);
/// Picks the encoder from the extension (.png or .bmp) and writes the file.
bool write_image(const Bitmap& bitmap, const std::string& path_utf8);

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0);

}  // namespace cvd::gfx

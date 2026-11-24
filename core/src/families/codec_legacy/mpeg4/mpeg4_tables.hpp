// Convertible core - codec_legacy family: MPEG-4 Part 2 (ISO/IEC 14496-2
// Annex B) and ITU-T H.263 tables.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

#include "families/codec_legacy/common/vlc.hpp"

namespace cvt::legacy::mpeg4 {

inline constexpr std::int32_t kStuffing = 1000;  // MCBPC stuffing
inline constexpr std::int32_t kEscape = 1001;    // TCOEF escape

/// MCBPC symbols: mb_type * 4 + cbpc.
[[nodiscard]] constexpr std::int32_t mcbpc(int type, int cbpc) noexcept { return type * 4 + cbpc; }

// H.263 Table 8 / MPEG-4 Table B-6: MCBPC for I pictures (mb_type 3 intra, 4 intra+q).
inline constexpr VlcCode kMcbpcI[] = {
    vlc("1", mcbpc(3, 0)),       vlc("001", mcbpc(3, 1)),     vlc("010", mcbpc(3, 2)),
    vlc("011", mcbpc(3, 3)),     vlc("0001", mcbpc(4, 0)),    vlc("000001", mcbpc(4, 1)),
    vlc("000010", mcbpc(4, 2)),  vlc("000011", mcbpc(4, 3)),  vlc("000000001", kStuffing)};

// H.263 Table 7 / MPEG-4 Table B-7: MCBPC for P pictures (0 inter, 1 inter+q,
// 2 inter4v, 3 intra, 4 intra+q, 5 inter4v+q (H.263 Annex... modified quant only)).
inline constexpr VlcCode kMcbpcP[] = {
    vlc("1", mcbpc(0, 0)),           vlc("0011", mcbpc(0, 1)),        vlc("0010", mcbpc(0, 2)),
    vlc("000101", mcbpc(0, 3)),      vlc("011", mcbpc(1, 0)),         vlc("0000111", mcbpc(1, 1)),
    vlc("0000110", mcbpc(1, 2)),     vlc("000000101", mcbpc(1, 3)),   vlc("010", mcbpc(2, 0)),
    vlc("0000101", mcbpc(2, 1)),     vlc("0000100", mcbpc(2, 2)),     vlc("00000101", mcbpc(2, 3)),
    vlc("00011", mcbpc(3, 0)),       vlc("00000100", mcbpc(3, 1)),    vlc("00000011", mcbpc(3, 2)),
    vlc("0000011", mcbpc(3, 3)),     vlc("000100", mcbpc(4, 0)),      vlc("000000100", mcbpc(4, 1)),
    vlc("000000011", mcbpc(4, 2)),   vlc("000000010", mcbpc(4, 3)),   vlc("000000001", kStuffing),
    vlc("00000000010", mcbpc(5, 0)), vlc("0000000001100", mcbpc(5, 1)), vlc("0000000001110", mcbpc(5, 2)),
    vlc("0000000001111", mcbpc(5, 3))};

// H.263 Table 13 / MPEG-4 Table B-8: CBPY (intra values; inter = 15 - value).
inline constexpr VlcCode kCbpy[] = {
    vlc("0011", 0), vlc("00101", 1), vlc("00100", 2), vlc("1001", 3), vlc("00011", 4), vlc("0111", 5),
    vlc("000010", 6), vlc("1011", 7), vlc("00010", 8), vlc("000011", 9), vlc("0101", 10), vlc("1010", 11),
    vlc("0100", 12), vlc("1000", 13), vlc("0110", 14), vlc("11", 15)};

// H.263 Table 14 / MPEG-4 Table B-12: motion vector differences, 0..32
// (a sign bit follows every non-zero code).
inline constexpr VlcCode kMvd[] = {
    vlc("1", 0), vlc("01", 1), vlc("001", 2), vlc("0001", 3), vlc("000011", 4), vlc("0000101", 5),
    vlc("0000100", 6), vlc("0000011", 7), vlc("000001011", 8), vlc("000001010", 9), vlc("000001001", 10),
    vlc("0000010001", 11), vlc("0000010000", 12), vlc("0000001111", 13), vlc("0000001110", 14),
    vlc("0000001101", 15), vlc("0000001100", 16), vlc("0000001011", 17), vlc("0000001010", 18),
    vlc("0000001001", 19), vlc("0000001000", 20), vlc("0000000111", 21), vlc("0000000110", 22),
    vlc("0000000101", 23), vlc("0000000100", 24), vlc("00000000111", 25), vlc("00000000110", 26),
    vlc("00000000101", 27), vlc("00000000100", 28), vlc("00000000011", 29), vlc("00000000010", 30),
    vlc("000000000011", 31), vlc("000000000010", 32)};

// MPEG-4 Tables B-13 and B-14: dct_dc_size for luminance and chrominance.
inline constexpr VlcCode kDcSizeLuma[] = {
    vlc("011", 0), vlc("11", 1), vlc("10", 2), vlc("010", 3), vlc("001", 4), vlc("0001", 5), vlc("00001", 6),
    vlc("000001", 7), vlc("0000001", 8), vlc("00000001", 9), vlc("000000001", 10), vlc("0000000001", 11),
    vlc("00000000001", 12)};
inline constexpr VlcCode kDcSizeChroma[] = {
    vlc("11", 0), vlc("10", 1), vlc("01", 2), vlc("001", 3), vlc("0001", 4), vlc("00001", 5), vlc("000001", 6),
    vlc("0000001", 7), vlc("00000001", 8), vlc("000000001", 9), vlc("0000000001", 10), vlc("00000000001", 11),
    vlc("000000000001", 12)};

/// TCOEF symbol: last << 12 | run << 6 | level.
[[nodiscard]] constexpr std::int32_t tc(int last, int run, int level) noexcept { return (last << 12) | (run << 6) | level; }

// H.263 Table 16 / MPEG-4 Table B-17: TCOEF for inter blocks (and all
// blocks of H.263 without Annex I). Codes without the sign bit.
inline constexpr VlcCode kTcoefInter[] = {
    vlc("10", tc(0, 0, 1)), vlc("1111", tc(0, 0, 2)), vlc("010101", tc(0, 0, 3)), vlc("0010111", tc(0, 0, 4)),
    vlc("00011111", tc(0, 0, 5)), vlc("000100101", tc(0, 0, 6)), vlc("000100100", tc(0, 0, 7)),
    vlc("0000100001", tc(0, 0, 8)), vlc("0000100000", tc(0, 0, 9)), vlc("00000000111", tc(0, 0, 10)),
    vlc("00000000110", tc(0, 0, 11)), vlc("00000100000", tc(0, 0, 12)), vlc("110", tc(0, 1, 1)),
    vlc("010100", tc(0, 1, 2)), vlc("00011110", tc(0, 1, 3)), vlc("0000001111", tc(0, 1, 4)),
    vlc("00000100001", tc(0, 1, 5)), vlc("000001010000", tc(0, 1, 6)), vlc("1110", tc(0, 2, 1)),
    vlc("00011101", tc(0, 2, 2)), vlc("0000001110", tc(0, 2, 3)), vlc("000001010001", tc(0, 2, 4)),
    vlc("01101", tc(0, 3, 1)), vlc("000100011", tc(0, 3, 2)), vlc("0000001101", tc(0, 3, 3)),
    vlc("01100", tc(0, 4, 1)), vlc("000100010", tc(0, 4, 2)), vlc("000001010010", tc(0, 4, 3)),
    vlc("01011", tc(0, 5, 1)), vlc("0000001100", tc(0, 5, 2)), vlc("000001010011", tc(0, 5, 3)),
    vlc("010011", tc(0, 6, 1)), vlc("0000001011", tc(0, 6, 2)), vlc("000001010100", tc(0, 6, 3)),
    vlc("010010", tc(0, 7, 1)), vlc("0000001010", tc(0, 7, 2)), vlc("010001", tc(0, 8, 1)),
    vlc("0000001001", tc(0, 8, 2)), vlc("010000", tc(0, 9, 1)), vlc("0000001000", tc(0, 9, 2)),
    vlc("0010110", tc(0, 10, 1)), vlc("000001010101", tc(0, 10, 2)), vlc("0010101", tc(0, 11, 1)),
    vlc("0010100", tc(0, 12, 1)), vlc("00011100", tc(0, 13, 1)), vlc("00011011", tc(0, 14, 1)),
    vlc("000100001", tc(0, 15, 1)), vlc("000100000", tc(0, 16, 1)), vlc("000011111", tc(0, 17, 1)),
    vlc("000011110", tc(0, 18, 1)), vlc("000011101", tc(0, 19, 1)), vlc("000011100", tc(0, 20, 1)),
    vlc("000011011", tc(0, 21, 1)), vlc("000011010", tc(0, 22, 1)), vlc("00000100010", tc(0, 23, 1)),
    vlc("00000100011", tc(0, 24, 1)), vlc("000001010110", tc(0, 25, 1)), vlc("000001010111", tc(0, 26, 1)),
    vlc("0111", tc(1, 0, 1)), vlc("000011001", tc(1, 0, 2)), vlc("00000000101", tc(1, 0, 3)),
    vlc("001111", tc(1, 1, 1)), vlc("00000000100", tc(1, 1, 2)), vlc("001110", tc(1, 2, 1)),
    vlc("001101", tc(1, 3, 1)), vlc("001100", tc(1, 4, 1)), vlc("0010011", tc(1, 5, 1)),
    vlc("0010010", tc(1, 6, 1)), vlc("0010001", tc(1, 7, 1)), vlc("0010000", tc(1, 8, 1)),
    vlc("00011010", tc(1, 9, 1)), vlc("00011001", tc(1, 10, 1)), vlc("00011000", tc(1, 11, 1)),
    vlc("00010111", tc(1, 12, 1)), vlc("00010110", tc(1, 13, 1)), vlc("00010101", tc(1, 14, 1)),
    vlc("00010100", tc(1, 15, 1)), vlc("00010011", tc(1, 16, 1)), vlc("000011000", tc(1, 17, 1)),
    vlc("000010111", tc(1, 18, 1)), vlc("000010110", tc(1, 19, 1)), vlc("000010101", tc(1, 20, 1)),
    vlc("000010100", tc(1, 21, 1)), vlc("000010011", tc(1, 22, 1)), vlc("000010010", tc(1, 23, 1)),
    vlc("000010001", tc(1, 24, 1)), vlc("0000000111", tc(1, 25, 1)), vlc("0000000110", tc(1, 26, 1)),
    vlc("0000000101", tc(1, 27, 1)), vlc("0000000100", tc(1, 28, 1)), vlc("00000100100", tc(1, 29, 1)),
    vlc("00000100101", tc(1, 30, 1)), vlc("00000100110", tc(1, 31, 1)), vlc("00000100111", tc(1, 32, 1)),
    vlc("000001011000", tc(1, 33, 1)), vlc("000001011001", tc(1, 34, 1)), vlc("000001011010", tc(1, 35, 1)),
    vlc("000001011011", tc(1, 36, 1)), vlc("000001011100", tc(1, 37, 1)), vlc("000001011101", tc(1, 38, 1)),
    vlc("000001011110", tc(1, 39, 1)), vlc("000001011111", tc(1, 40, 1)), vlc("0000011", kEscape)};

// MPEG-4 Table B-16: TCOEF for intra blocks (the same code words as B-17
// assigned to other events). Codes without the sign bit.
inline constexpr VlcCode kTcoefIntra[] = {
    vlc("10", tc(0, 0, 1)), vlc("110", tc(0, 0, 2)), vlc("1111", tc(0, 0, 3)), vlc("01101", tc(0, 0, 4)),
    vlc("01100", tc(0, 0, 5)), vlc("010101", tc(0, 0, 6)), vlc("010011", tc(0, 0, 7)), vlc("010010", tc(0, 0, 8)),
    vlc("0010111", tc(0, 0, 9)), vlc("00011111", tc(0, 0, 10)), vlc("00011110", tc(0, 0, 11)),
    vlc("00011101", tc(0, 0, 12)), vlc("000100101", tc(0, 0, 13)), vlc("000100100", tc(0, 0, 14)),
    vlc("000100011", tc(0, 0, 15)), vlc("000100001", tc(0, 0, 16)), vlc("0000100001", tc(0, 0, 17)),
    vlc("0000100000", tc(0, 0, 18)), vlc("0000001111", tc(0, 0, 19)), vlc("0000001110", tc(0, 0, 20)),
    vlc("00000000111", tc(0, 0, 21)), vlc("00000000110", tc(0, 0, 22)), vlc("00000100000", tc(0, 0, 23)),
    vlc("00000100001", tc(0, 0, 24)), vlc("000001010000", tc(0, 0, 25)), vlc("000001010001", tc(0, 0, 26)),
    vlc("000001010010", tc(0, 0, 27)), vlc("1110", tc(0, 1, 1)), vlc("010100", tc(0, 1, 2)),
    vlc("0010110", tc(0, 1, 3)), vlc("00011100", tc(0, 1, 4)), vlc("000100000", tc(0, 1, 5)),
    vlc("000011111", tc(0, 1, 6)), vlc("0000001101", tc(0, 1, 7)), vlc("00000100010", tc(0, 1, 8)),
    vlc("000001010011", tc(0, 1, 9)), vlc("000001010101", tc(0, 1, 10)), vlc("01011", tc(0, 2, 1)),
    vlc("0010101", tc(0, 2, 2)), vlc("000011110", tc(0, 2, 3)), vlc("0000001100", tc(0, 2, 4)),
    vlc("000001010110", tc(0, 2, 5)), vlc("010001", tc(0, 3, 1)), vlc("00011011", tc(0, 3, 2)),
    vlc("000011101", tc(0, 3, 3)), vlc("0000001011", tc(0, 3, 4)), vlc("010000", tc(0, 4, 1)),
    vlc("000100010", tc(0, 4, 2)), vlc("0000001010", tc(0, 4, 3)), vlc("001101", tc(0, 5, 1)),
    vlc("000011100", tc(0, 5, 2)), vlc("0000001000", tc(0, 5, 3)), vlc("0010010", tc(0, 6, 1)),
    vlc("000011011", tc(0, 6, 2)), vlc("000001010100", tc(0, 6, 3)), vlc("0010100", tc(0, 7, 1)),
    vlc("000011010", tc(0, 7, 2)), vlc("000001010111", tc(0, 7, 3)), vlc("00011001", tc(0, 8, 1)),
    vlc("0000001001", tc(0, 8, 2)), vlc("00011000", tc(0, 9, 1)), vlc("00000100011", tc(0, 9, 2)),
    vlc("00010111", tc(0, 10, 1)), vlc("000011001", tc(0, 11, 1)), vlc("000011000", tc(0, 12, 1)),
    vlc("0000000111", tc(0, 13, 1)), vlc("000001011000", tc(0, 14, 1)), vlc("0111", tc(1, 0, 1)),
    vlc("001100", tc(1, 0, 2)), vlc("00010110", tc(1, 0, 3)), vlc("000010111", tc(1, 0, 4)),
    vlc("0000000110", tc(1, 0, 5)), vlc("00000000101", tc(1, 0, 6)), vlc("00000000100", tc(1, 0, 7)),
    vlc("000001011001", tc(1, 0, 8)), vlc("001111", tc(1, 1, 1)), vlc("000010110", tc(1, 1, 2)),
    vlc("0000000101", tc(1, 1, 3)), vlc("001110", tc(1, 2, 1)), vlc("0000000100", tc(1, 2, 2)),
    vlc("0010001", tc(1, 3, 1)), vlc("00000100100", tc(1, 3, 2)), vlc("0010000", tc(1, 4, 1)),
    vlc("00000100101", tc(1, 4, 2)), vlc("0010011", tc(1, 5, 1)), vlc("000001011010", tc(1, 5, 2)),
    vlc("00010101", tc(1, 6, 1)), vlc("000001011011", tc(1, 6, 2)), vlc("00010100", tc(1, 7, 1)),
    vlc("00010011", tc(1, 8, 1)), vlc("00011010", tc(1, 9, 1)), vlc("000010101", tc(1, 10, 1)),
    vlc("000010100", tc(1, 11, 1)), vlc("000010011", tc(1, 12, 1)), vlc("000010010", tc(1, 13, 1)),
    vlc("000010001", tc(1, 14, 1)), vlc("00000100110", tc(1, 15, 1)), vlc("00000100111", tc(1, 16, 1)),
    vlc("000001011100", tc(1, 17, 1)), vlc("000001011101", tc(1, 18, 1)), vlc("000001011110", tc(1, 19, 1)),
    vlc("000001011111", tc(1, 20, 1)), vlc("0000011", kEscape)};

// MPEG-4 Table B-3 (modb) and B-4 (B-VOP mb_type).
inline constexpr VlcCode kModb[] = {vlc("1", 0), vlc("01", 1), vlc("00", 2)};
inline constexpr int kBDirect = 0;
inline constexpr int kBInterpolate = 1;
inline constexpr int kBBackward = 2;
inline constexpr int kBForward = 3;
inline constexpr VlcCode kBMbType[] = {vlc("1", kBDirect), vlc("01", kBInterpolate), vlc("001", kBBackward),
                                       vlc("0001", kBForward)};

// MPEG-4 Table V2-? : sprite trajectory dmv_length.
inline constexpr VlcCode kDmvLength[] = {
    vlc("00", 0), vlc("010", 1), vlc("011", 2), vlc("100", 3), vlc("101", 4), vlc("110", 5), vlc("1110", 6),
    vlc("11110", 7), vlc("111110", 8), vlc("1111110", 9), vlc("11111110", 10), vlc("111111110", 11),
    vlc("1111111110", 12), vlc("11111111110", 13), vlc("111111111110", 14)};

// MPEG-4 default quantiser matrices (raster order).
inline constexpr std::uint8_t kDefaultIntra[64] = {
    8,  17, 18, 19, 21, 23, 25, 27, 17, 18, 19, 21, 23, 25, 27, 28, 20, 21, 22, 23, 24, 26,
    28, 30, 21, 22, 23, 24, 26, 28, 30, 32, 22, 23, 24, 26, 28, 30, 32, 35, 23, 24, 26, 28,
    30, 32, 35, 38, 25, 26, 28, 30, 32, 35, 38, 41, 27, 28, 30, 32, 35, 38, 41, 45};
inline constexpr std::uint8_t kDefaultInter[64] = {
    16, 17, 18, 19, 20, 21, 22, 23, 17, 18, 19, 20, 21, 22, 23, 24, 18, 19, 20, 21, 22, 23,
    24, 25, 19, 20, 21, 22, 23, 24, 26, 27, 20, 21, 22, 23, 25, 26, 27, 28, 21, 22, 23, 24,
    26, 27, 28, 30, 22, 23, 24, 26, 27, 28, 30, 31, 23, 24, 25, 27, 28, 30, 31, 33};

// Alternate horizontal scan (ISO/IEC 14496-2 Figure 7-5), scan -> raster.
inline constexpr std::uint8_t kAltHorizontalScan[64] = {
    0,  1,  2,  3,  8,  9,  16, 17, 10, 11, 4,  5,  6,  7,  15, 14, 13, 12, 19, 18, 24, 25,
    32, 33, 26, 27, 20, 21, 22, 23, 28, 29, 30, 31, 34, 35, 40, 41, 48, 49, 42, 43, 36, 37,
    38, 39, 44, 45, 46, 47, 50, 51, 56, 57, 58, 59, 52, 53, 54, 55, 60, 61, 62, 63};

/// DC scaler (ISO/IEC 14496-2 Table 7-1).
[[nodiscard]] constexpr int dc_scaler(int q, bool chroma) noexcept {
  if (!chroma) {
    if (q <= 4) return 8;
    if (q <= 8) return 2 * q;
    if (q <= 24) return q + 8;
    return 2 * q - 16;
  }
  if (q <= 4) return 8;
  if (q <= 24) return (q + 13) / 2;
  return q - 6;
}

}  // namespace cvt::legacy::mpeg4

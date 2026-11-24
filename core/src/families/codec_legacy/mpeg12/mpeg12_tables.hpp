// Convertible core - codec_legacy family: MPEG-1/MPEG-2 video tables
// (ISO/IEC 11172-2 and ISO/IEC 13818-2 Annex B).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>

#include "families/codec_legacy/common/vlc.hpp"

namespace cvt::legacy::mpeg12 {

// Start codes (the byte after 00 00 01).
inline constexpr std::uint8_t kPictureStart = 0x00;
inline constexpr std::uint8_t kSliceMin = 0x01;
inline constexpr std::uint8_t kSliceMax = 0xAF;
inline constexpr std::uint8_t kUserData = 0xB2;
inline constexpr std::uint8_t kSequenceHeader = 0xB3;
inline constexpr std::uint8_t kSequenceError = 0xB4;
inline constexpr std::uint8_t kExtension = 0xB5;
inline constexpr std::uint8_t kSequenceEnd = 0xB7;
inline constexpr std::uint8_t kGroupStart = 0xB8;

// macroblock_type flags.
inline constexpr int kMbQuant = 1;
inline constexpr int kMbForward = 2;
inline constexpr int kMbBackward = 4;
inline constexpr int kMbPattern = 8;
inline constexpr int kMbIntra = 16;

// Special symbols.
inline constexpr int kMbaEscape = 34;
inline constexpr int kMbaStuffing = 35;
inline constexpr int kDctEob = 1000;
inline constexpr int kDctEscape = 1001;

// Table B-1: macroblock_address_increment.
inline constexpr VlcCode kMbaCodes[] = {
    vlc("1", 1), vlc("011", 2), vlc("010", 3), vlc("0011", 4), vlc("0010", 5), vlc("00011", 6), vlc("00010", 7),
    vlc("0000111", 8), vlc("0000110", 9), vlc("00001011", 10), vlc("00001010", 11), vlc("00001001", 12),
    vlc("00001000", 13), vlc("00000111", 14), vlc("00000110", 15), vlc("0000010111", 16), vlc("0000010110", 17),
    vlc("0000010101", 18), vlc("0000010100", 19), vlc("0000010011", 20), vlc("0000010010", 21),
    vlc("00000100011", 22), vlc("00000100010", 23), vlc("00000100001", 24), vlc("00000100000", 25),
    vlc("00000011111", 26), vlc("00000011110", 27), vlc("00000011101", 28), vlc("00000011100", 29),
    vlc("00000011011", 30), vlc("00000011010", 31), vlc("00000011001", 32), vlc("00000011000", 33),
    vlc("00000001000", kMbaEscape), vlc("00000001111", kMbaStuffing)};

// Tables B-2, B-3, B-4: macroblock_type for I, P and B pictures.
inline constexpr VlcCode kMbTypeI[] = {vlc("1", kMbIntra), vlc("01", kMbIntra | kMbQuant)};
inline constexpr VlcCode kMbTypeP[] = {
    vlc("1", kMbForward | kMbPattern), vlc("01", kMbPattern), vlc("001", kMbForward), vlc("00011", kMbIntra),
    vlc("00010", kMbForward | kMbPattern | kMbQuant), vlc("00001", kMbPattern | kMbQuant),
    vlc("000001", kMbIntra | kMbQuant)};
inline constexpr VlcCode kMbTypeB[] = {
    vlc("10", kMbForward | kMbBackward), vlc("11", kMbForward | kMbBackward | kMbPattern), vlc("010", kMbBackward),
    vlc("011", kMbBackward | kMbPattern), vlc("0010", kMbForward), vlc("0011", kMbForward | kMbPattern),
    vlc("00011", kMbIntra), vlc("00010", kMbForward | kMbBackward | kMbPattern | kMbQuant),
    vlc("000011", kMbForward | kMbPattern | kMbQuant), vlc("000010", kMbBackward | kMbPattern | kMbQuant),
    vlc("000001", kMbIntra | kMbQuant)};
inline constexpr VlcCode kMbTypeD[] = {vlc("1", kMbIntra)};

// Table B-9: coded_block_pattern (value 0 only in MPEG-2).
inline constexpr VlcCode kCbpCodes[] = {
    vlc("111", 60), vlc("1101", 4), vlc("1100", 8), vlc("1011", 16), vlc("1010", 32), vlc("10011", 12),
    vlc("10010", 48), vlc("10001", 20), vlc("10000", 40), vlc("01111", 28), vlc("01110", 44), vlc("01101", 52),
    vlc("01100", 56), vlc("01011", 1), vlc("01010", 61), vlc("01001", 2), vlc("01000", 62), vlc("001111", 24),
    vlc("001110", 36), vlc("001101", 3), vlc("001100", 63), vlc("0010111", 5), vlc("0010110", 9),
    vlc("0010101", 17), vlc("0010100", 33), vlc("0010011", 6), vlc("0010010", 10), vlc("0010001", 18),
    vlc("0010000", 34), vlc("00011111", 7), vlc("00011110", 11), vlc("00011101", 19), vlc("00011100", 35),
    vlc("00011011", 13), vlc("00011010", 49), vlc("00011001", 21), vlc("00011000", 41), vlc("00010111", 14),
    vlc("00010110", 50), vlc("00010101", 22), vlc("00010100", 42), vlc("00010011", 15), vlc("00010010", 51),
    vlc("00010001", 23), vlc("00010000", 43), vlc("00001111", 25), vlc("00001110", 37), vlc("00001101", 26),
    vlc("00001100", 38), vlc("00001011", 29), vlc("00001010", 45), vlc("00001001", 53), vlc("00001000", 57),
    vlc("00000111", 30), vlc("00000110", 46), vlc("00000101", 54), vlc("00000100", 58), vlc("000000111", 31),
    vlc("000000110", 47), vlc("000000101", 55), vlc("000000100", 59), vlc("000000011", 27), vlc("000000010", 39),
    vlc("000000001", 0)};

// Table B-10: motion_code magnitudes (a sign bit follows every non-zero code).
inline constexpr VlcCode kMotionCodes[] = {
    vlc("1", 0), vlc("01", 1), vlc("001", 2), vlc("0001", 3), vlc("000011", 4), vlc("0000101", 5),
    vlc("0000100", 6), vlc("0000011", 7), vlc("000001011", 8), vlc("000001010", 9), vlc("000001001", 10),
    vlc("0000010001", 11), vlc("0000010000", 12), vlc("0000001111", 13), vlc("0000001110", 14),
    vlc("0000001101", 15), vlc("0000001100", 16)};

// Table B-11: dmvector.
inline constexpr VlcCode kDmvCodes[] = {vlc("0", 0), vlc("10", 1), vlc("11", -1)};

// Tables B-12 and B-13: dct_dc_size.
inline constexpr VlcCode kDcLumaCodes[] = {
    vlc("100", 0), vlc("00", 1), vlc("01", 2), vlc("101", 3), vlc("110", 4), vlc("1110", 5), vlc("11110", 6),
    vlc("111110", 7), vlc("1111110", 8), vlc("11111110", 9), vlc("111111110", 10), vlc("111111111", 11)};
inline constexpr VlcCode kDcChromaCodes[] = {
    vlc("00", 0), vlc("01", 1), vlc("10", 2), vlc("110", 3), vlc("1110", 4), vlc("11110", 5), vlc("111110", 6),
    vlc("1111110", 7), vlc("11111110", 8), vlc("111111110", 9), vlc("1111111110", 10), vlc("1111111111", 11)};

/// DCT coefficient symbol: run * 64 + level (level 1..40); a sign bit follows.
[[nodiscard]] constexpr std::int32_t rl(int run, int level) noexcept { return run * 64 + level; }

// Table B-14 (DCT coefficients table zero), without the sign bits. "1" for
// (0,1) as the first coefficient of a non-intra block is handled by the caller.
inline constexpr VlcCode kDctZero[] = {
    vlc("10", kDctEob), vlc("11", rl(0, 1)), vlc("011", rl(1, 1)), vlc("0100", rl(0, 2)), vlc("0101", rl(2, 1)),
    vlc("00101", rl(0, 3)), vlc("00111", rl(3, 1)), vlc("00110", rl(4, 1)), vlc("000110", rl(1, 2)),
    vlc("000111", rl(5, 1)), vlc("000101", rl(6, 1)), vlc("000100", rl(7, 1)), vlc("0000110", rl(0, 4)),
    vlc("0000100", rl(2, 2)), vlc("0000111", rl(8, 1)), vlc("0000101", rl(9, 1)), vlc("000001", kDctEscape),
    vlc("00100110", rl(0, 5)), vlc("00100001", rl(0, 6)), vlc("00100101", rl(1, 3)), vlc("00100100", rl(3, 2)),
    vlc("00100111", rl(10, 1)), vlc("00100011", rl(11, 1)), vlc("00100010", rl(12, 1)), vlc("00100000", rl(13, 1)),
    vlc("0000001010", rl(0, 7)), vlc("0000001100", rl(1, 4)), vlc("0000001011", rl(2, 3)),
    vlc("0000001111", rl(4, 2)), vlc("0000001001", rl(5, 2)), vlc("0000001110", rl(14, 1)),
    vlc("0000001101", rl(15, 1)), vlc("0000001000", rl(16, 1)), vlc("000000011101", rl(0, 8)),
    vlc("000000011000", rl(0, 9)), vlc("000000010011", rl(0, 10)), vlc("000000010000", rl(0, 11)),
    vlc("000000011011", rl(1, 5)), vlc("000000010100", rl(2, 4)), vlc("000000011100", rl(3, 3)),
    vlc("000000010010", rl(4, 3)), vlc("000000011110", rl(6, 2)), vlc("000000010101", rl(7, 2)),
    vlc("000000010001", rl(8, 2)), vlc("000000011111", rl(17, 1)), vlc("000000011010", rl(18, 1)),
    vlc("000000011001", rl(19, 1)), vlc("000000010111", rl(20, 1)), vlc("000000010110", rl(21, 1)),
    vlc("0000000011010", rl(0, 12)), vlc("0000000011001", rl(0, 13)), vlc("0000000011000", rl(0, 14)),
    vlc("0000000010111", rl(0, 15)), vlc("0000000010110", rl(1, 6)), vlc("0000000010101", rl(1, 7)),
    vlc("0000000010100", rl(2, 5)), vlc("0000000010011", rl(3, 4)), vlc("0000000010010", rl(5, 3)),
    vlc("0000000010001", rl(9, 2)), vlc("0000000010000", rl(10, 2)), vlc("0000000011111", rl(22, 1)),
    vlc("0000000011110", rl(23, 1)), vlc("0000000011101", rl(24, 1)), vlc("0000000011100", rl(25, 1)),
    vlc("0000000011011", rl(26, 1)), vlc("00000000011111", rl(0, 16)), vlc("00000000011110", rl(0, 17)),
    vlc("00000000011101", rl(0, 18)), vlc("00000000011100", rl(0, 19)), vlc("00000000011011", rl(0, 20)),
    vlc("00000000011010", rl(0, 21)), vlc("00000000011001", rl(0, 22)), vlc("00000000011000", rl(0, 23)),
    vlc("00000000010111", rl(0, 24)), vlc("00000000010110", rl(0, 25)), vlc("00000000010101", rl(0, 26)),
    vlc("00000000010100", rl(0, 27)), vlc("00000000010011", rl(0, 28)), vlc("00000000010010", rl(0, 29)),
    vlc("00000000010001", rl(0, 30)), vlc("00000000010000", rl(0, 31)), vlc("000000000011000", rl(0, 32)),
    vlc("000000000010111", rl(0, 33)), vlc("000000000010110", rl(0, 34)), vlc("000000000010101", rl(0, 35)),
    vlc("000000000010100", rl(0, 36)), vlc("000000000010011", rl(0, 37)), vlc("000000000010010", rl(0, 38)),
    vlc("000000000010001", rl(0, 39)), vlc("000000000010000", rl(0, 40)), vlc("000000000011111", rl(1, 8)),
    vlc("000000000011110", rl(1, 9)), vlc("000000000011101", rl(1, 10)), vlc("000000000011100", rl(1, 11)),
    vlc("000000000011011", rl(1, 12)), vlc("000000000011010", rl(1, 13)), vlc("000000000011001", rl(1, 14)),
    vlc("0000000000010011", rl(1, 15)), vlc("0000000000010010", rl(1, 16)), vlc("0000000000010001", rl(1, 17)),
    vlc("0000000000010000", rl(1, 18)), vlc("0000000000010100", rl(6, 3)), vlc("0000000000011010", rl(11, 2)),
    vlc("0000000000011001", rl(12, 2)), vlc("0000000000011000", rl(13, 2)), vlc("0000000000010111", rl(14, 2)),
    vlc("0000000000010110", rl(15, 2)), vlc("0000000000010101", rl(16, 2)), vlc("0000000000011111", rl(27, 1)),
    vlc("0000000000011110", rl(28, 1)), vlc("0000000000011101", rl(29, 1)), vlc("0000000000011100", rl(30, 1)),
    vlc("0000000000011011", rl(31, 1))};

// Table B-15 (DCT coefficients table one, intra_vlc_format = 1), without sign bits.
inline constexpr VlcCode kDctOne[] = {
    vlc("0110", kDctEob), vlc("10", rl(0, 1)), vlc("010", rl(1, 1)), vlc("110", rl(0, 2)), vlc("00101", rl(2, 1)),
    vlc("0111", rl(0, 3)), vlc("00111", rl(3, 1)), vlc("000110", rl(4, 1)), vlc("00110", rl(1, 2)),
    vlc("000111", rl(5, 1)), vlc("0000110", rl(6, 1)), vlc("0000100", rl(7, 1)), vlc("11100", rl(0, 4)),
    vlc("0000111", rl(2, 2)), vlc("0000101", rl(8, 1)), vlc("1111000", rl(9, 1)), vlc("000001", kDctEscape),
    vlc("11101", rl(0, 5)), vlc("000101", rl(0, 6)), vlc("1111001", rl(1, 3)), vlc("00100110", rl(3, 2)),
    vlc("1111010", rl(10, 1)), vlc("00100001", rl(11, 1)), vlc("00100101", rl(12, 1)), vlc("00100100", rl(13, 1)),
    vlc("000100", rl(0, 7)), vlc("00100111", rl(1, 4)), vlc("11111100", rl(2, 3)), vlc("11111101", rl(4, 2)),
    vlc("000000100", rl(5, 2)), vlc("000000101", rl(14, 1)), vlc("000000111", rl(15, 1)),
    vlc("0000001101", rl(16, 1)), vlc("1111011", rl(0, 8)), vlc("1111100", rl(0, 9)), vlc("00100011", rl(0, 10)),
    vlc("00100010", rl(0, 11)), vlc("00100000", rl(1, 5)), vlc("0000001100", rl(2, 4)),
    vlc("000000011100", rl(3, 3)), vlc("000000010010", rl(4, 3)), vlc("000000011110", rl(6, 2)),
    vlc("000000010101", rl(7, 2)), vlc("000000010001", rl(8, 2)), vlc("000000011111", rl(17, 1)),
    vlc("000000011010", rl(18, 1)), vlc("000000011001", rl(19, 1)), vlc("000000010111", rl(20, 1)),
    vlc("000000010110", rl(21, 1)), vlc("11111010", rl(0, 12)), vlc("11111011", rl(0, 13)),
    vlc("11111110", rl(0, 14)), vlc("11111111", rl(0, 15)), vlc("0000000010110", rl(1, 6)),
    vlc("0000000010101", rl(1, 7)), vlc("0000000010100", rl(2, 5)), vlc("0000000010011", rl(3, 4)),
    vlc("0000000010010", rl(5, 3)), vlc("0000000010001", rl(9, 2)), vlc("0000000010000", rl(10, 2)),
    vlc("0000000011111", rl(22, 1)), vlc("0000000011110", rl(23, 1)), vlc("0000000011101", rl(24, 1)),
    vlc("0000000011100", rl(25, 1)), vlc("0000000011011", rl(26, 1)), vlc("00000000011111", rl(0, 16)),
    vlc("00000000011110", rl(0, 17)), vlc("00000000011101", rl(0, 18)), vlc("00000000011100", rl(0, 19)),
    vlc("00000000011011", rl(0, 20)), vlc("00000000011010", rl(0, 21)), vlc("00000000011001", rl(0, 22)),
    vlc("00000000011000", rl(0, 23)), vlc("00000000010111", rl(0, 24)), vlc("00000000010110", rl(0, 25)),
    vlc("00000000010101", rl(0, 26)), vlc("00000000010100", rl(0, 27)), vlc("00000000010011", rl(0, 28)),
    vlc("00000000010010", rl(0, 29)), vlc("00000000010001", rl(0, 30)), vlc("00000000010000", rl(0, 31)),
    vlc("000000000011000", rl(0, 32)), vlc("000000000010111", rl(0, 33)), vlc("000000000010110", rl(0, 34)),
    vlc("000000000010101", rl(0, 35)), vlc("000000000010100", rl(0, 36)), vlc("000000000010011", rl(0, 37)),
    vlc("000000000010010", rl(0, 38)), vlc("000000000010001", rl(0, 39)), vlc("000000000010000", rl(0, 40)),
    vlc("000000000011111", rl(1, 8)), vlc("000000000011110", rl(1, 9)), vlc("000000000011101", rl(1, 10)),
    vlc("000000000011100", rl(1, 11)), vlc("000000000011011", rl(1, 12)), vlc("000000000011010", rl(1, 13)),
    vlc("000000000011001", rl(1, 14)), vlc("0000000000010011", rl(1, 15)), vlc("0000000000010010", rl(1, 16)),
    vlc("0000000000010001", rl(1, 17)), vlc("0000000000010000", rl(1, 18)), vlc("0000000000010100", rl(6, 3)),
    vlc("0000000000011010", rl(11, 2)), vlc("0000000000011001", rl(12, 2)), vlc("0000000000011000", rl(13, 2)),
    vlc("0000000000010111", rl(14, 2)), vlc("0000000000010110", rl(15, 2)), vlc("0000000000010101", rl(16, 2)),
    vlc("0000000000011111", rl(27, 1)), vlc("0000000000011110", rl(28, 1)), vlc("0000000000011101", rl(29, 1)),
    vlc("0000000000011100", rl(30, 1)), vlc("0000000000011011", rl(31, 1))};

// Default quantiser matrices, raster order.
inline constexpr std::uint8_t kDefaultIntraMatrix[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
    34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
    35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

// Non-linear quantiser_scale (q_scale_type = 1), Table 7-6.
inline constexpr std::uint8_t kNonLinearScale[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12,
                                                     14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44,
                                                     48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

// frame_rate_code -> frames per second (Table 6-4).
struct FrameRate {
  std::int64_t num;
  std::int64_t den;
};
inline constexpr FrameRate kFrameRates[16] = {{0, 1},     {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1},
                                             {50, 1},    {60000, 1001}, {60, 1}, {0, 1},  {0, 1},         {0, 1},
                                             {0, 1},     {0, 1},        {0, 1},  {0, 1}};

}  // namespace cvt::legacy::mpeg12

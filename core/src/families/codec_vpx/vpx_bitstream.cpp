// Convertible core - codec_vpx family: VP8/VP9 header parsing, VP9
// superframes and vpcC records.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/codec_vpx/vpx_bitstream.hpp"

#include <cstddef>
#include <cstdint>

namespace cvt::vpx {

namespace {

/// MSB-first bit reader over the uncompressed VP9 header. Reading past the
/// end sets `overrun` and yields zeros.
class BitReader {
 public:
  explicit BitReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  std::uint32_t bits(int count) noexcept {
    std::uint32_t value = 0;
    for (int i = 0; i < count; ++i) value = (value << 1) | bit();
    return value;
  }
  std::uint32_t bit() noexcept {
    if (position_ >= data_.size() * 8) {
      overrun_ = true;
      return 0;
    }
    const std::uint32_t value = (data_[position_ >> 3] >> (7 - (position_ & 7))) & 1u;
    ++position_;
    return value;
  }
  [[nodiscard]] bool overrun() const noexcept { return overrun_; }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t position_ = 0;
  bool overrun_ = false;
};

bool read_vp9_sync_code(BitReader& reader) {
  return reader.bits(8) == 0x49 && reader.bits(8) == 0x83 && reader.bits(8) == 0x42;
}

Result<void> read_vp9_color_config(BitReader& reader, FrameHeader& header) {
  header.has_color_config = true;
  header.bit_depth = header.profile >= 2 ? (reader.bit() != 0 ? 12 : 10) : 8;
  header.color_space = static_cast<Vp9ColorSpace>(reader.bits(3));
  if (header.color_space != Vp9ColorSpace::Srgb) {
    header.full_range = reader.bit() != 0;
    if (header.profile == 1 || header.profile == 3) {
      header.subsampling_x = static_cast<int>(reader.bit());
      header.subsampling_y = static_cast<int>(reader.bit());
      if (header.subsampling_x == 1 && header.subsampling_y == 1) {
        return fail(Status::Decode, "VP9 profile {} cannot carry 4:2:0 video", header.profile);
      }
      if (reader.bit() != 0) return fail(Status::Decode, "VP9 reserved bit is set");
    } else {
      header.subsampling_x = 1;
      header.subsampling_y = 1;
    }
  } else {
    header.full_range = true;
    if (header.profile == 1 || header.profile == 3) {
      header.subsampling_x = 0;
      header.subsampling_y = 0;
      if (reader.bit() != 0) return fail(Status::Decode, "VP9 reserved bit is set");
    } else {
      return fail(Status::Decode, "VP9 profile {} cannot carry RGB video", header.profile);
    }
  }
  return {};
}

void read_vp9_frame_size(BitReader& reader, FrameHeader& header) {
  header.width = reader.bits(16) + 1;
  header.height = reader.bits(16) + 1;
}

void skip_vp9_render_size(BitReader& reader) {
  if (reader.bit() != 0) (void)reader.bits(32);
}

}  // namespace

Result<FrameHeader> parse_vp8_header(ByteView frame) {
  const std::span<const std::uint8_t> data = as_u8(frame);
  if (data.size() < 3) return fail(Status::Decode, "VP8 frame is truncated");
  const std::uint32_t tag = data[0] | (std::uint32_t{data[1]} << 8) | (std::uint32_t{data[2]} << 16);
  FrameHeader header;
  header.keyframe = (tag & 1u) == 0;
  header.profile = static_cast<int>((tag >> 1) & 7u);
  header.show_frame = ((tag >> 4) & 1u) != 0;
  const std::uint32_t first_partition = tag >> 5;
  if (header.profile > 3) return fail(Status::Decode, "unsupported VP8 version {}", header.profile);
  if (header.keyframe) {
    if (data.size() < 10) return fail(Status::Decode, "VP8 keyframe header is truncated");
    if (data[3] != 0x9D || data[4] != 0x01 || data[5] != 0x2A) {
      return fail(Status::Decode, "VP8 keyframe start code is missing");
    }
    header.width = (data[6] | (std::uint32_t{data[7]} << 8)) & 0x3FFFu;
    header.height = (data[8] | (std::uint32_t{data[9]} << 8)) & 0x3FFFu;
    if (header.width == 0 || header.height == 0) return fail(Status::Decode, "VP8 keyframe has no pixels");
    if (first_partition > data.size() - 10) return fail(Status::Decode, "VP8 first partition is truncated");
  } else if (first_partition > data.size() - 3) {
    return fail(Status::Decode, "VP8 first partition is truncated");
  }
  header.has_color_config = header.keyframe;  // VP8 is always 8-bit 4:2:0, BT.601
  header.color_space = Vp9ColorSpace::Bt601;
  return header;
}

Result<FrameHeader> parse_vp9_header(ByteView frame, Vp9RefSizes* refs) {
  BitReader reader(as_u8(frame));
  FrameHeader header;
  if (reader.bits(2) != 2) return fail(Status::Decode, "VP9 frame marker is missing");
  const std::uint32_t low = reader.bit();
  const std::uint32_t high = reader.bit();
  header.profile = static_cast<int>((high << 1) | low);
  if (header.profile == 3 && reader.bit() != 0) return fail(Status::Decode, "VP9 reserved bit is set");
  header.show_existing_frame = reader.bit() != 0;
  if (header.show_existing_frame) {
    const std::uint32_t slot = reader.bits(3);
    if (reader.overrun()) return fail(Status::Decode, "VP9 frame header is truncated");
    if (refs != nullptr) {
      header.width = refs->width[slot];
      header.height = refs->height[slot];
    }
    header.show_frame = true;
    return header;
  }
  header.keyframe = reader.bit() == 0;
  header.show_frame = reader.bit() != 0;
  const bool error_resilient = reader.bit() != 0;
  if (header.keyframe) {
    if (!read_vp9_sync_code(reader)) return fail(Status::Decode, "VP9 keyframe sync code is missing");
    CVT_TRY(read_vp9_color_config(reader, header));
    read_vp9_frame_size(reader, header);
    skip_vp9_render_size(reader);
    header.refresh_frame_flags = 0xFF;
  } else {
    header.intra_only = header.show_frame ? false : reader.bit() != 0;
    if (!error_resilient) (void)reader.bits(2);  // reset_frame_context
    if (header.intra_only) {
      if (!read_vp9_sync_code(reader)) return fail(Status::Decode, "VP9 intra-only sync code is missing");
      if (header.profile > 0) {
        CVT_TRY(read_vp9_color_config(reader, header));
      } else {
        header.has_color_config = true;
        header.bit_depth = 8;
        header.color_space = Vp9ColorSpace::Bt601;
        header.subsampling_x = 1;
        header.subsampling_y = 1;
      }
      header.refresh_frame_flags = static_cast<std::uint8_t>(reader.bits(8));
      read_vp9_frame_size(reader, header);
      skip_vp9_render_size(reader);
    } else {
      header.refresh_frame_flags = static_cast<std::uint8_t>(reader.bits(8));
      std::array<std::uint32_t, 3> ref_slots{};
      for (std::uint32_t& slot : ref_slots) {
        slot = reader.bits(3);
        (void)reader.bit();  // sign bias
      }
      bool found = false;
      for (const std::uint32_t slot : ref_slots) {
        if (reader.bit() != 0) {
          if (refs != nullptr) {
            header.width = refs->width[slot];
            header.height = refs->height[slot];
          }
          found = true;
          break;
        }
      }
      if (!found) read_vp9_frame_size(reader, header);
      skip_vp9_render_size(reader);
    }
  }
  if (reader.overrun()) return fail(Status::Decode, "VP9 frame header is truncated");
  if (refs != nullptr) {
    for (std::size_t slot = 0; slot < 8; ++slot) {
      if ((header.refresh_frame_flags >> slot) & 1u) {
        refs->width[slot] = header.width;
        refs->height[slot] = header.height;
      }
    }
  }
  return header;
}

Result<std::vector<ByteView>> split_vp9_superframe(ByteView packet) {
  std::vector<ByteView> frames;
  const std::span<const std::uint8_t> data = as_u8(packet);
  if (data.empty()) return frames;
  const std::uint8_t marker = data.back();
  if ((marker & 0xE0u) == 0xC0u) {
    const std::size_t count = (marker & 7u) + 1;
    const std::size_t magnitude = ((marker >> 3) & 3u) + 1;
    const std::size_t index_size = 2 + magnitude * count;
    if (data.size() >= index_size && data[data.size() - index_size] == marker) {
      std::size_t at = data.size() - index_size + 1;
      std::size_t offset = 0;
      const std::size_t payload = data.size() - index_size;
      for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t size = 0;
        for (std::size_t b = 0; b < magnitude; ++b) size |= std::uint64_t{data[at++]} << (8 * b);
        if (size > payload - offset) return fail(Status::Decode, "VP9 superframe index exceeds the packet");
        // A zero-sized entry is legal (and skipped) only as a placeholder.
        if (size != 0) frames.push_back(packet.subspan(offset, static_cast<std::size_t>(size)));
        offset += static_cast<std::size_t>(size);
      }
      return frames;
    }
  }
  frames.push_back(packet);
  return frames;
}

Bytes make_vpcc(const VpcConfig& config) {
  util::ByteWriter out(12);
  out.u8(1);  // version
  out.u8(0);  // flags (24 bits)
  out.u8(0);
  out.u8(0);
  out.u8(config.profile);
  out.u8(config.level);
  out.u8(static_cast<std::uint8_t>(((config.bit_depth & 0x0Fu) << 4) | ((config.chroma_subsampling & 7u) << 1) |
                                   (config.full_range ? 1u : 0u)));
  out.u8(config.colour_primaries);
  out.u8(config.transfer_characteristics);
  out.u8(config.matrix_coefficients);
  out.u8(0);  // codecInitializationDataSize (16 bits): none for VP8/VP9
  out.u8(0);
  return out.take();
}

Result<VpcConfig> parse_vpcc(ByteView payload) {
  util::ByteReader reader(payload);
  const std::uint8_t version = reader.u8();
  (void)reader.u24be();
  if (!reader.ok()) return fail(Status::Decode, "vpcC record is truncated");
  if (version != 1) return fail(Status::Decode, "unsupported vpcC version {}", version);
  VpcConfig config;
  config.profile = reader.u8();
  config.level = reader.u8();
  const std::uint8_t packed = reader.u8();
  config.bit_depth = static_cast<std::uint8_t>(packed >> 4);
  config.chroma_subsampling = static_cast<std::uint8_t>((packed >> 1) & 7u);
  config.full_range = (packed & 1u) != 0;
  config.colour_primaries = reader.u8();
  config.transfer_characteristics = reader.u8();
  config.matrix_coefficients = reader.u8();
  if (!reader.ok()) return fail(Status::Decode, "vpcC record is truncated");
  return config;
}

std::uint8_t vp9_level(std::uint32_t width, std::uint32_t height, double fps) {
  struct Level {
    std::uint8_t level;
    std::uint64_t max_luma_sample_rate;
    std::uint64_t max_luma_picture_size;
    std::uint32_t max_dimension;
  };
  // VP9 levels (webmproject.org/vp9/levels).
  static constexpr Level levels[] = {
      {10, 829'440, 36'864, 512},           {11, 2'764'800, 73'728, 768},
      {20, 4'608'000, 122'880, 960},        {21, 9'216'000, 245'760, 1'344},
      {30, 20'736'000, 552'960, 2'048},     {31, 36'864'000, 983'040, 2'752},
      {40, 83'558'400, 2'228'224, 4'160},   {41, 160'432'128, 2'228'224, 4'160},
      {50, 311'951'360, 8'912'896, 8'384},  {51, 588'251'136, 8'912'896, 8'384},
      {52, 1'176'502'272, 8'912'896, 8'384}, {60, 1'176'502'272, 35'651'584, 16'832},
      {61, 2'353'004'544, 35'651'584, 16'832}, {62, 4'706'009'088, 35'651'584, 16'832},
  };
  const std::uint64_t picture = std::uint64_t{width} * height;
  const double rate = fps > 0 ? static_cast<double>(picture) * fps : 0.0;
  for (const Level& level : levels) {
    if (picture <= level.max_luma_picture_size && width <= level.max_dimension && height <= level.max_dimension &&
        rate <= static_cast<double>(level.max_luma_sample_rate)) {
      return level.level;
    }
  }
  return 0;
}

H273Colour h273_colour(ir::ColorMatrix matrix, int bit_depth) {
  switch (matrix) {
    case ir::ColorMatrix::BT601: return {6, 6, 6};
    case ir::ColorMatrix::BT709: return {1, 1, 1};
    case ir::ColorMatrix::BT2020:
      return {9, static_cast<std::uint8_t>(bit_depth >= 12 ? 15 : 14), 9};
  }
  return {};
}

}  // namespace cvt::vpx

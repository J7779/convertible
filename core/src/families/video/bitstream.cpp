// Convertible core - video family: elementary-stream helpers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/video/bitstream.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>

namespace cvt::video::bits {

namespace {

std::uint8_t at(ByteView data, std::size_t i) noexcept { return std::to_integer<std::uint8_t>(data[i]); }

void put_be16(Bytes& out, std::uint32_t v) {
  out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
  out.push_back(static_cast<std::byte>(v & 0xFF));
}
void put_u8(Bytes& out, std::uint32_t v) { out.push_back(static_cast<std::byte>(v & 0xFF)); }

bool contains(const std::vector<Bytes>& list, ByteView item) {
  return std::ranges::any_of(list, [&](const Bytes& b) {
    return b.size() == item.size() && std::equal(b.begin(), b.end(), item.begin());
  });
}

}  // namespace

// --------------------------------------------------------------------------- bit reader / writer

std::uint32_t BitReader::bits(int count) noexcept {
  std::uint32_t value = 0;
  for (int i = 0; i < count; ++i) {
    if (pos_ >= data_.size() * 8) {
      failed_ = true;
      return 0;
    }
    const std::uint8_t byte = at(data_, static_cast<std::size_t>(pos_ >> 3));
    value = (value << 1) | ((byte >> (7 - (pos_ & 7))) & 1u);
    ++pos_;
  }
  return value;
}

std::uint32_t BitReader::ue() noexcept {
  int zeros = 0;
  while (!failed_ && bits(1) == 0) {
    if (++zeros > 31) {
      failed_ = true;
      return 0;
    }
  }
  if (failed_) return 0;
  if (zeros == 0) return 0;
  return static_cast<std::uint32_t>((std::uint64_t{1} << zeros) - 1 + bits(zeros));
}

std::int32_t BitReader::se() noexcept {
  const std::uint32_t k = ue();
  const auto magnitude = static_cast<std::int32_t>((k + 1) / 2);
  return (k & 1u) ? magnitude : -magnitude;
}

void BitReader::skip(std::uint64_t count) noexcept {
  if (count > remaining()) {
    failed_ = true;
    pos_ = data_.size() * 8;
    return;
  }
  pos_ += count;
}

void BitWriter::put(std::uint32_t value, int count) {
  for (int i = count - 1; i >= 0; --i) {
    acc_ = (acc_ << 1) | ((value >> i) & 1u);
    if (++used_ == 8) {
      out_.push_back(static_cast<std::byte>(acc_ & 0xFF));
      acc_ = 0;
      used_ = 0;
    }
  }
}

void BitWriter::align_zero() {
  while (used_ != 0) put(0, 1);
}

Bytes BitWriter::take() {
  align_zero();
  return std::exchange(out_, {});
}

Bytes unescape_rbsp(ByteView nal) {
  Bytes out;
  out.reserve(nal.size());
  int zeros = 0;
  for (std::size_t i = 0; i < nal.size(); ++i) {
    const std::uint8_t b = at(nal, i);
    if (zeros >= 2 && b == 3) {
      zeros = 0;
      continue;
    }
    zeros = b == 0 ? zeros + 1 : 0;
    out.push_back(nal[i]);
  }
  return out;
}

// --------------------------------------------------------------------------- NAL units

bool starts_with_start_code(ByteView data) noexcept {
  if (data.size() >= 3 && at(data, 0) == 0 && at(data, 1) == 0 && at(data, 2) == 1) return true;
  return data.size() >= 4 && at(data, 0) == 0 && at(data, 1) == 0 && at(data, 2) == 0 && at(data, 3) == 1;
}

std::vector<ByteView> split_annexb(ByteView data) {
  std::vector<ByteView> nals;
  const std::size_t n = data.size();
  std::size_t i = 0;
  // Find the first start code.
  auto find_start = [&](std::size_t from) -> std::size_t {
    for (std::size_t k = from; k + 2 < n; ++k) {
      if (at(data, k) == 0 && at(data, k + 1) == 0 && at(data, k + 2) == 1) return k;
    }
    return n;
  };
  i = find_start(0);
  while (i < n) {
    const std::size_t begin = i + 3;
    std::size_t next = find_start(begin);
    std::size_t end = next;
    while (end > begin && at(data, end - 1) == 0) --end;  // trailing zeros / 4-byte start code
    if (end > begin) nals.push_back(data.subspan(begin, end - begin));
    i = next;
  }
  return nals;
}

std::optional<std::vector<ByteView>> split_length_prefixed(ByteView data, int length_size) {
  if (length_size < 1 || length_size > 4) return std::nullopt;
  std::vector<ByteView> nals;
  std::size_t i = 0;
  const auto size = static_cast<std::size_t>(length_size);
  while (i < data.size()) {
    if (data.size() - i < size) return std::nullopt;
    std::uint64_t length = 0;
    for (std::size_t k = 0; k < size; ++k) length = (length << 8) | at(data, i + k);
    i += size;
    if (length > data.size() - i) return std::nullopt;
    if (length > 0) nals.push_back(data.subspan(i, static_cast<std::size_t>(length)));
    i += static_cast<std::size_t>(length);
  }
  return nals;
}

Bytes to_length_prefixed(const std::vector<ByteView>& nals) {
  Bytes out;
  std::size_t total = 0;
  for (const ByteView nal : nals) total += nal.size() + 4;
  out.reserve(total);
  for (const ByteView nal : nals) {
    const auto len = static_cast<std::uint32_t>(nal.size());
    put_u8(out, len >> 24);
    put_u8(out, len >> 16);
    put_u8(out, len >> 8);
    put_u8(out, len);
    out.insert(out.end(), nal.begin(), nal.end());
  }
  return out;
}

Bytes to_annexb(const std::vector<ByteView>& nals) {
  Bytes out;
  for (const ByteView nal : nals) {
    put_u8(out, 0);
    put_u8(out, 0);
    put_u8(out, 0);
    put_u8(out, 1);
    out.insert(out.end(), nal.begin(), nal.end());
  }
  return out;
}

std::optional<ParameterSets> parse_avcc(ByteView data) {
  util::ByteReader r(data);
  if (r.u8() != 1) return std::nullopt;
  r.skip(3);
  ParameterSets sets;
  sets.length_size = (r.u8() & 3) + 1;
  const int sps_count = r.u8() & 0x1F;
  for (int i = 0; i < sps_count; ++i) {
    const std::uint16_t len = r.u16be();
    const ByteView nal = r.bytes(len);
    if (!r.ok()) return std::nullopt;
    sets.sps.emplace_back(nal.begin(), nal.end());
  }
  const int pps_count = r.u8();
  for (int i = 0; i < pps_count; ++i) {
    const std::uint16_t len = r.u16be();
    const ByteView nal = r.bytes(len);
    if (!r.ok()) return std::nullopt;
    sets.pps.emplace_back(nal.begin(), nal.end());
  }
  if (!r.ok()) return std::nullopt;
  return sets;
}

std::optional<ParameterSets> parse_hvcc(ByteView data) {
  util::ByteReader r(data);
  if (data.size() < 23) return std::nullopt;
  r.skip(21);
  ParameterSets sets;
  sets.length_size = (r.u8() & 3) + 1;
  const int arrays = r.u8();
  for (int a = 0; a < arrays; ++a) {
    const int type = r.u8() & 0x3F;
    const int count = r.u16be();
    for (int i = 0; i < count; ++i) {
      const std::uint16_t len = r.u16be();
      const ByteView nal = r.bytes(len);
      if (!r.ok()) return std::nullopt;
      if (type == 32) sets.vps.emplace_back(nal.begin(), nal.end());
      if (type == 33) sets.sps.emplace_back(nal.begin(), nal.end());
      if (type == 34) sets.pps.emplace_back(nal.begin(), nal.end());
    }
  }
  if (!r.ok()) return std::nullopt;
  return sets;
}

Result<Bytes> build_avcc(const ParameterSets& sets) {
  if (sets.sps.empty() || sets.sps.front().size() < 4) return fail(Status::Encode, "H.264 stream has no sequence parameter set");
  const Bytes& sps = sets.sps.front();
  Bytes out;
  put_u8(out, 1);
  out.push_back(sps[1]);
  out.push_back(sps[2]);
  out.push_back(sps[3]);
  put_u8(out, 0xFF);  // 4-byte lengths
  put_u8(out, 0xE0 | static_cast<std::uint32_t>(std::min<std::size_t>(sets.sps.size(), 31)));
  for (std::size_t i = 0; i < sets.sps.size() && i < 31; ++i) {
    put_be16(out, static_cast<std::uint32_t>(sets.sps[i].size()));
    out.insert(out.end(), sets.sps[i].begin(), sets.sps[i].end());
  }
  put_u8(out, static_cast<std::uint32_t>(std::min<std::size_t>(sets.pps.size(), 255)));
  for (std::size_t i = 0; i < sets.pps.size() && i < 255; ++i) {
    put_be16(out, static_cast<std::uint32_t>(sets.pps[i].size()));
    out.insert(out.end(), sets.pps[i].begin(), sets.pps[i].end());
  }
  const int profile = std::to_integer<int>(sps[1]);
  if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
    const auto info = parse_h264_sps(sps);
    const int chroma = info ? info->chroma_format : 1;
    const int luma = info ? info->bit_depth_luma : 8;
    const int chroma_depth = info ? info->bit_depth_chroma : 8;
    put_u8(out, 0xFC | static_cast<std::uint32_t>(chroma));
    put_u8(out, 0xF8 | static_cast<std::uint32_t>(luma - 8));
    put_u8(out, 0xF8 | static_cast<std::uint32_t>(chroma_depth - 8));
    put_u8(out, 0);
  }
  return out;
}

Result<Bytes> build_hvcc(const ParameterSets& sets) {
  if (sets.vps.empty() || sets.sps.empty() || sets.pps.empty()) {
    return fail(Status::Encode, "HEVC stream lacks its parameter sets (VPS/SPS/PPS)");
  }
  const Bytes sps = unescape_rbsp(sets.sps.front());
  if (sps.size() < 15) return fail(Status::Encode, "HEVC sequence parameter set is truncated");
  const auto info = parse_hevc_sps(sets.sps.front());
  int sub_layers = (std::to_integer<int>(sps[2]) >> 1) & 7;
  const bool nested = (std::to_integer<int>(sps[2]) & 1) != 0;
  Bytes out;
  put_u8(out, 1);
  out.insert(out.end(), sps.begin() + 3, sps.begin() + 15);  // profile_space .. general_level_idc
  put_be16(out, 0xF000);
  put_u8(out, 0xFC);
  put_u8(out, 0xFC | static_cast<std::uint32_t>(info ? info->chroma_format : 1));
  put_u8(out, 0xF8 | static_cast<std::uint32_t>((info ? info->bit_depth_luma : 8) - 8));
  put_u8(out, 0xF8 | static_cast<std::uint32_t>((info ? info->bit_depth_chroma : 8) - 8));
  put_be16(out, 0);
  put_u8(out, static_cast<std::uint32_t>(((sub_layers + 1) << 3) | (nested ? 4 : 0) | 3));
  put_u8(out, 3);
  const auto array = [&](int type, const std::vector<Bytes>& list) {
    put_u8(out, 0x80 | static_cast<std::uint32_t>(type));
    put_be16(out, static_cast<std::uint32_t>(list.size()));
    for (const Bytes& nal : list) {
      put_be16(out, static_cast<std::uint32_t>(nal.size()));
      out.insert(out.end(), nal.begin(), nal.end());
    }
  };
  array(32, sets.vps);
  array(33, sets.sps);
  array(34, sets.pps);
  return out;
}

void collect_parameter_sets(const std::vector<ByteView>& nals, bool hevc, ParameterSets& sets) {
  for (const ByteView nal : nals) {
    if (nal.size() > 0xFFFF) continue;
    if (hevc) {
      const int type = hevc_nal_type(nal);
      std::vector<Bytes>* list = type == 32 ? &sets.vps : type == 33 ? &sets.sps : type == 34 ? &sets.pps : nullptr;
      if (list != nullptr && !contains(*list, nal)) list->emplace_back(nal.begin(), nal.end());
    } else {
      const int type = h264_nal_type(nal);
      std::vector<Bytes>* list = type == 7 ? &sets.sps : type == 8 ? &sets.pps : nullptr;
      if (list != nullptr && !contains(*list, nal)) list->emplace_back(nal.begin(), nal.end());
    }
  }
}

namespace {

/// Table E-1 of H.264 / H.265: sample aspect ratios by aspect_ratio_idc.
constexpr std::array<std::array<std::uint16_t, 2>, 17> sar_table = {{{0, 0},
                                                                    {1, 1},
                                                                    {12, 11},
                                                                    {10, 11},
                                                                    {16, 11},
                                                                    {40, 33},
                                                                    {24, 11},
                                                                    {20, 11},
                                                                    {32, 11},
                                                                    {80, 33},
                                                                    {18, 11},
                                                                    {15, 11},
                                                                    {64, 33},
                                                                    {160, 99},
                                                                    {4, 3},
                                                                    {3, 2},
                                                                    {2, 1}}};

void read_sar(BitReader& r, SpsInfo& info) {
  const std::uint32_t idc = r.bits(8);
  if (idc == 255) {
    info.sar_num = r.bits(16);
    info.sar_den = r.bits(16);
  } else if (idc < sar_table.size()) {
    info.sar_num = sar_table[idc][0];
    info.sar_den = sar_table[idc][1];
  }
}

void skip_h264_hrd(BitReader& r) {
  const std::uint32_t count = r.ue();
  if (count > 31) {
    r.skip(r.remaining() + 1);
    return;
  }
  r.skip(8);
  for (std::uint32_t i = 0; i <= count && !r.failed(); ++i) {
    r.ue();
    r.ue();
    r.skip(1);
  }
  r.skip(20);
}

/// H.264 vui_parameters() (Annex E.1.1): aspect ratio, timing, reordering.
void parse_h264_vui(BitReader& r, SpsInfo& info) {
  if (r.flag()) read_sar(r, info);
  if (r.flag()) r.skip(1);  // overscan
  if (r.flag()) {           // video_signal_type
    r.skip(4);
    if (r.flag()) r.skip(24);
  }
  if (r.flag()) {  // chroma_loc_info
    r.ue();
    r.ue();
  }
  if (r.flag()) {  // timing_info_present_flag
    const std::uint32_t units = r.bits(32);
    const std::uint32_t scale = r.bits(32);
    r.skip(1);  // fixed_frame_rate_flag
    // One frame = two field ticks.
    if (!r.failed() && units > 0 && scale > 0) {
      info.fps_num = scale;
      info.fps_den = units > 0x7FFFFFFFu ? 0 : units * 2;
      if (info.fps_den == 0) info.fps_num = 0;
    }
  }
  const bool nal_hrd = r.flag();
  if (nal_hrd) skip_h264_hrd(r);
  const bool vcl_hrd = r.flag();
  if (vcl_hrd) skip_h264_hrd(r);
  if (nal_hrd || vcl_hrd) r.skip(1);  // low_delay_hrd_flag
  r.skip(1);                          // pic_struct_present_flag
  if (r.flag()) {                     // bitstream_restriction_flag
    r.skip(1);
    r.ue();
    r.ue();
    r.ue();
    r.ue();
    const std::uint32_t reorder = r.ue();
    r.ue();
    if (!r.failed() && reorder <= 16) info.max_num_reorder = reorder;
  }
}

}  // namespace

std::optional<SpsInfo> parse_h264_sps(ByteView nal) {
  if (nal.size() < 4 || h264_nal_type(nal) != 7) return std::nullopt;
  const Bytes rbsp = unescape_rbsp(nal.subspan(1));
  BitReader r(rbsp);
  SpsInfo info;
  info.profile = static_cast<int>(r.bits(8));
  r.bits(8);
  info.level = static_cast<int>(r.bits(8));
  r.ue();
  const int p = info.profile;
  if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 ||
      p == 138 || p == 139 || p == 134 || p == 135) {
    info.chroma_format = static_cast<int>(r.ue());
    if (info.chroma_format == 3) r.bits(1);
    info.bit_depth_luma = static_cast<int>(r.ue()) + 8;
    info.bit_depth_chroma = static_cast<int>(r.ue()) + 8;
    r.bits(1);
    if (r.flag()) {
      const int lists = info.chroma_format != 3 ? 8 : 12;
      for (int i = 0; i < lists && !r.failed(); ++i) {
        if (!r.flag()) continue;
        const int size = i < 6 ? 16 : 64;
        int last = 8;
        int next = 8;
        for (int j = 0; j < size && !r.failed(); ++j) {
          if (next != 0) next = (last + r.se() + 256) % 256;
          last = next == 0 ? last : next;
        }
      }
    }
  }
  r.ue();  // log2_max_frame_num_minus4
  const std::uint32_t poc_type = r.ue();
  if (poc_type == 0) {
    r.ue();
  } else if (poc_type == 1) {
    r.bits(1);
    r.se();
    r.se();
    const std::uint32_t cycle = r.ue();
    if (cycle > 255) return std::nullopt;
    for (std::uint32_t i = 0; i < cycle; ++i) r.se();
  }
  r.ue();
  r.bits(1);
  const std::uint32_t width_mbs = r.ue() + 1;
  const std::uint32_t height_units = r.ue() + 1;
  const bool frame_mbs_only = r.flag();
  if (!frame_mbs_only) r.bits(1);
  r.bits(1);
  std::uint32_t crop[4] = {0, 0, 0, 0};
  if (r.flag()) {
    for (auto& c : crop) c = r.ue();
  }
  if (r.failed() || width_mbs > 4096 || height_units > 4096) return std::nullopt;
  const std::uint32_t sub_w = info.chroma_format == 1 || info.chroma_format == 2 ? 2 : 1;
  const std::uint32_t sub_h = info.chroma_format == 1 ? 2 : 1;
  const std::uint32_t unit_x = info.chroma_format == 0 ? 1 : sub_w;
  const std::uint32_t unit_y = (info.chroma_format == 0 ? 1 : sub_h) * (frame_mbs_only ? 1 : 2);
  const std::uint64_t w = std::uint64_t{width_mbs} * 16;
  const std::uint64_t h = std::uint64_t{height_units} * 16 * (frame_mbs_only ? 1 : 2);
  const std::uint64_t cx = std::uint64_t{unit_x} * (crop[0] + std::uint64_t{crop[1]});
  const std::uint64_t cy = std::uint64_t{unit_y} * (crop[2] + std::uint64_t{crop[3]});
  if (cx >= w || cy >= h) return std::nullopt;
  info.width = static_cast<std::uint32_t>(w - cx);
  info.height = static_cast<std::uint32_t>(h - cy);
  info.frame_mbs_only = frame_mbs_only;
  if (r.flag()) parse_h264_vui(r, info);  // vui_parameters_present_flag; best effort
  return info;
}

namespace {

struct PocSps {
  int log2_max_frame_num = 4;
  int poc_type = 0;
  int log2_max_poc_lsb = 4;
  bool frame_mbs_only = true;
  bool separate_colour_plane = false;
};

struct PocPps {
  int sps_id = 0;
  bool bottom_field_pic_order = false;
};

std::optional<std::pair<int, PocSps>> parse_poc_sps(ByteView nal) {
  const Bytes rbsp = unescape_rbsp(nal.subspan(1));
  BitReader r(rbsp);
  const int profile = static_cast<int>(r.bits(8));
  r.bits(16);
  const int id = static_cast<int>(r.ue());
  PocSps s;
  if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
      profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
      profile == 135) {
    const std::uint32_t chroma = r.ue();
    if (chroma == 3) s.separate_colour_plane = r.flag();
    r.ue();
    r.ue();
    r.bits(1);
    if (r.flag()) {
      for (int i = 0; i < (chroma != 3 ? 8 : 12) && !r.failed(); ++i) {
        if (!r.flag()) continue;
        int last = 8;
        int next = 8;
        for (int j = 0; j < (i < 6 ? 16 : 64) && !r.failed(); ++j) {
          if (next != 0) next = (last + r.se() + 256) % 256;
          last = next == 0 ? last : next;
        }
      }
    }
  }
  s.log2_max_frame_num = static_cast<int>(r.ue()) + 4;
  s.poc_type = static_cast<int>(r.ue());
  if (s.poc_type == 0) {
    s.log2_max_poc_lsb = static_cast<int>(r.ue()) + 4;
  } else if (s.poc_type == 1) {
    return std::nullopt;
  }
  r.ue();
  r.bits(1);
  r.ue();
  r.ue();
  s.frame_mbs_only = r.flag();
  if (r.failed() || id > 31 || s.log2_max_frame_num > 16 || s.log2_max_poc_lsb > 16) return std::nullopt;
  return std::pair{id, s};
}

}  // namespace

struct H264PocTracker::State {
  std::map<int, PocSps> sps;
  std::map<int, PocPps> pps;
  std::int64_t prev_msb = 0;
  std::int64_t prev_lsb = 0;
  std::int64_t frame_offset = 0;
  std::int64_t prev_frame_num = 0;
};

H264PocTracker::H264PocTracker() : state_(std::make_unique<State>()) {}
H264PocTracker::~H264PocTracker() = default;

std::optional<std::vector<std::pair<std::int64_t, bool>>> h264_picture_order(
    const std::vector<std::vector<ByteView>>& access_units) {
  H264PocTracker tracker;
  std::vector<std::pair<std::int64_t, bool>> out;
  for (const std::vector<ByteView>& au : access_units) {
    auto poc = tracker.next(au);
    if (!poc) return std::nullopt;
    out.push_back(*poc);
  }
  return out;
}

std::optional<std::pair<std::int64_t, bool>> H264PocTracker::next(const std::vector<ByteView>& au) {
  auto& sps = state_->sps;
  auto& pps = state_->pps;
  std::int64_t& prev_msb = state_->prev_msb;
  std::int64_t& prev_lsb = state_->prev_lsb;
  std::int64_t& frame_offset = state_->frame_offset;
  std::int64_t& prev_frame_num = state_->prev_frame_num;
  {
    std::optional<std::pair<std::int64_t, bool>> result;
    for (const ByteView nal : au) {
      const int type = h264_nal_type(nal);
      if (type == 7) {
        if (auto s = parse_poc_sps(nal)) sps[s->first] = s->second;
      } else if (type == 8) {
        const Bytes rbsp = unescape_rbsp(nal.subspan(1));
        BitReader r(rbsp);
        const int id = static_cast<int>(r.ue());
        PocPps p;
        p.sps_id = static_cast<int>(r.ue());
        r.bits(1);
        p.bottom_field_pic_order = r.flag();
        if (!r.failed() && id < 256) pps[id] = p;
      } else if ((type == 1 || type == 5) && !result) {
        const Bytes rbsp = unescape_rbsp(nal.subspan(1, std::min<std::size_t>(nal.size() - 1, 64)));
        BitReader r(rbsp);
        r.ue();
        r.ue();
        const int pps_id = static_cast<int>(r.ue());
        const auto p = pps.find(pps_id);
        if (p == pps.end()) return std::nullopt;
        const auto s = sps.find(p->second.sps_id);
        if (s == sps.end()) return std::nullopt;
        const PocSps& q = s->second;
        if (q.separate_colour_plane) r.bits(2);
        const std::int64_t frame_num = r.bits(q.log2_max_frame_num);
        bool field = false;
        if (!q.frame_mbs_only) {
          field = r.flag();
          if (field) r.bits(1);
        }
        const bool idr = type == 5;
        if (idr) r.ue();
        const bool reference = (std::to_integer<int>(nal[0]) & 0x60) != 0;
        std::int64_t poc = 0;
        if (q.poc_type == 0) {
          const std::int64_t lsb = r.bits(q.log2_max_poc_lsb);
          const std::int64_t max = std::int64_t{1} << q.log2_max_poc_lsb;
          if (idr) {
            prev_msb = 0;
            prev_lsb = 0;
          }
          std::int64_t msb = prev_msb;
          if (lsb < prev_lsb && prev_lsb - lsb >= max / 2) msb = prev_msb + max;
          if (lsb > prev_lsb && lsb - prev_lsb > max / 2) msb = prev_msb - max;
          poc = msb + lsb;
          if (reference) {
            prev_msb = msb;
            prev_lsb = lsb;
          }
        } else {
          // Type 2: output order equals decode order.
          const std::int64_t max_frame = std::int64_t{1} << q.log2_max_frame_num;
          if (idr) {
            frame_offset = 0;
          } else if (prev_frame_num > frame_num) {
            frame_offset += max_frame;
          }
          poc = 2 * (frame_offset + frame_num) - (reference ? 0 : 1);
          prev_frame_num = frame_num;
        }
        if (r.failed()) return std::nullopt;
        (void)field;
        result = std::pair{poc, idr};
      }
    }
    return result;
  }
}

bool h264_is_keyframe(const std::vector<ByteView>& nals) noexcept {
  return std::ranges::any_of(nals, [](ByteView nal) { return h264_nal_type(nal) == 5; });
}

bool hevc_is_keyframe(const std::vector<ByteView>& nals) noexcept {
  return std::ranges::any_of(nals, [](ByteView nal) {
    const int t = hevc_nal_type(nal);
    return t >= 16 && t <= 23;
  });
}

// --------------------------------------------------------------------------- AAC

namespace {
constexpr std::array<std::uint32_t, 13> aac_rates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                     22050, 16000, 12000, 11025, 8000,  7350};
}

int aac_sampling_index(std::uint32_t rate) noexcept {
  for (std::size_t i = 0; i < aac_rates.size(); ++i) {
    if (aac_rates[i] == rate) return static_cast<int>(i);
  }
  return -1;
}

std::uint32_t aac_sample_rate(int index) noexcept {
  return index >= 0 && index < static_cast<int>(aac_rates.size()) ? aac_rates[static_cast<std::size_t>(index)] : 0;
}

std::optional<AdtsHeader> parse_adts(ByteView data) noexcept {
  if (data.size() < 7) return std::nullopt;
  if (at(data, 0) != 0xFF || (at(data, 1) & 0xF6) != 0xF0) return std::nullopt;
  AdtsHeader h;
  const bool protection_absent = (at(data, 1) & 1) != 0;
  h.config.object_type = ((at(data, 2) >> 6) & 3) + 1;
  h.config.sampling_index = (at(data, 2) >> 2) & 0xF;
  h.config.sample_rate = aac_sample_rate(h.config.sampling_index);
  if (h.config.sample_rate == 0) return std::nullopt;
  h.config.channel_config = ((at(data, 2) & 1) << 2) | ((at(data, 3) >> 6) & 3);
  h.frame_size = (static_cast<std::size_t>(at(data, 3) & 3) << 11) | (static_cast<std::size_t>(at(data, 4)) << 3) |
                 (static_cast<std::size_t>(at(data, 5)) >> 5);
  h.header_size = protection_absent ? 7 : 9;
  if (h.frame_size < h.header_size) return std::nullopt;
  return h;
}

std::optional<AacConfig> parse_asc(ByteView data) noexcept {
  BitReader r(data);
  AacConfig c;
  auto object_type = [&]() {
    int t = static_cast<int>(r.bits(5));
    if (t == 31) t = 32 + static_cast<int>(r.bits(6));
    return t;
  };
  auto rate = [&](int& index) -> std::uint32_t {
    index = static_cast<int>(r.bits(4));
    if (index == 15) return r.bits(24);
    return aac_sample_rate(index);
  };
  c.object_type = object_type();
  c.sample_rate = rate(c.sampling_index);
  c.channel_config = static_cast<int>(r.bits(4));
  if (c.object_type == 5 || c.object_type == 29) {
    int ext_index = 0;
    const std::uint32_t ext_rate = rate(ext_index);
    if (ext_rate != 0) c.sample_rate = ext_rate;
    object_type();
  } else if ((c.object_type >= 1 && c.object_type <= 4) || c.object_type == 6 || c.object_type == 7 ||
             c.object_type == 17 || (c.object_type >= 19 && c.object_type <= 23)) {
    if (r.flag()) c.samples_per_frame = 960;
  }
  if (r.failed() || c.sample_rate == 0) return std::nullopt;
  return c;
}

Bytes build_asc(const AacConfig& config) {
  BitWriter w;
  w.put(static_cast<std::uint32_t>(config.object_type), 5);
  const int index = aac_sampling_index(config.sample_rate);
  if (index >= 0) {
    w.put(static_cast<std::uint32_t>(index), 4);
  } else {
    w.put(15, 4);
    w.put(config.sample_rate, 24);
  }
  w.put(static_cast<std::uint32_t>(config.channel_config), 4);
  w.put(config.samples_per_frame == 960 ? 1 : 0, 1);
  w.put(0, 2);  // dependsOnCoreCoder, extensionFlag
  return w.take();
}

Bytes build_adts_header(const AacConfig& config, std::size_t payload_size) {
  const std::size_t length = payload_size + 7;
  const int index = std::max(0, aac_sampling_index(config.sample_rate));
  const int profile = std::clamp(config.object_type - 1, 0, 3);
  Bytes h(7);
  h[0] = std::byte{0xFF};
  h[1] = std::byte{0xF1};
  h[2] = static_cast<std::byte>((profile << 6) | (index << 2) | ((config.channel_config >> 2) & 1));
  h[3] = static_cast<std::byte>(((config.channel_config & 3) << 6) | static_cast<int>((length >> 11) & 3));
  h[4] = static_cast<std::byte>((length >> 3) & 0xFF);
  h[5] = static_cast<std::byte>(((length & 7) << 5) | 0x1F);
  h[6] = std::byte{0xFC};
  return h;
}

// --------------------------------------------------------------------------- MPEG audio

std::optional<MpegAudioHeader> parse_mpeg_audio(ByteView data) noexcept {
  if (data.size() < 4) return std::nullopt;
  const std::uint32_t h = (static_cast<std::uint32_t>(at(data, 0)) << 24) | (static_cast<std::uint32_t>(at(data, 1)) << 16) |
                          (static_cast<std::uint32_t>(at(data, 2)) << 8) | at(data, 3);
  if ((h & 0xFFE00000u) != 0xFFE00000u) return std::nullopt;
  const std::uint32_t version_bits = (h >> 19) & 3;
  const std::uint32_t layer_bits = (h >> 17) & 3;
  const std::uint32_t bitrate_index = (h >> 12) & 0xF;
  const std::uint32_t rate_index = (h >> 10) & 3;
  if (version_bits == 1 || layer_bits == 0 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) {
    return std::nullopt;
  }
  MpegAudioHeader m;
  m.version = version_bits == 3 ? 1 : version_bits == 2 ? 2 : 25;
  m.layer = 4 - static_cast<int>(layer_bits);
  static constexpr std::array<std::array<std::uint16_t, 16>, 5> rates = {{
      {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},  // V1 L1
      {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},     // V1 L2
      {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},      // V1 L3
      {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},     // V2 L1
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},          // V2 L2/L3
  }};
  const std::size_t table = m.version == 1 ? static_cast<std::size_t>(m.layer - 1) : (m.layer == 1 ? 3 : 4);
  m.bitrate_kbps = rates[table][bitrate_index];
  static constexpr std::array<std::uint32_t, 3> base = {44100, 48000, 32000};
  m.sample_rate = base[rate_index] / (m.version == 1 ? 1 : m.version == 2 ? 2 : 4);
  const std::uint32_t padding = (h >> 9) & 1;
  m.channels = ((h >> 6) & 3) == 3 ? 1 : 2;
  if (m.layer == 1) {
    m.samples_per_frame = 384;
    m.frame_size = (12 * m.bitrate_kbps * 1000 / m.sample_rate + padding) * 4;
  } else {
    m.samples_per_frame = (m.layer == 3 && m.version != 1) ? 576 : 1152;
    const std::uint32_t factor = (m.layer == 3 && m.version != 1) ? 72 : 144;
    m.frame_size = factor * m.bitrate_kbps * 1000 / m.sample_rate + padding;
  }
  if (m.frame_size < 4) return std::nullopt;
  return m;
}

// --------------------------------------------------------------------------- AV1

namespace {

struct Obu {
  int type = 0;
  ByteView whole;    // header included
  ByteView payload;
};

std::optional<std::uint64_t> leb128(util::ByteReader& r) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    const std::uint8_t b = r.u8();
    if (!r.ok()) return std::nullopt;
    value |= static_cast<std::uint64_t>(b & 0x7F) << (7 * i);
    if ((b & 0x80) == 0) return value;
  }
  return std::nullopt;
}

std::vector<Obu> split_obus(ByteView data) {
  std::vector<Obu> obus;
  util::ByteReader r(data);
  while (!r.at_end()) {
    const std::size_t start = r.position();
    const std::uint8_t header = r.u8();
    if ((header & 0x80) != 0) break;
    Obu obu;
    obu.type = (header >> 3) & 0xF;
    if ((header & 4) != 0) r.skip(1);
    std::uint64_t size = 0;
    if ((header & 2) != 0) {
      const auto s = leb128(r);
      if (!s) break;
      size = *s;
    } else {
      size = r.remaining();
    }
    if (!r.ok() || size > r.remaining()) break;
    obu.payload = r.bytes(static_cast<std::size_t>(size));
    obu.whole = data.subspan(start, r.position() - start);
    obus.push_back(obu);
  }
  return obus;
}

struct Av1Sequence {
  int profile = 0;
  int level = 0;
  int tier = 0;
  bool high_bitdepth = false;
  bool twelve_bit = false;
  bool mono = false;
  int ss_x = 1;
  int ss_y = 1;
  int sample_position = 0;
  bool reduced = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

std::optional<Av1Sequence> parse_av1_sequence(ByteView payload) {
  BitReader r(payload);
  Av1Sequence s;
  s.profile = static_cast<int>(r.bits(3));
  r.bits(1);
  s.reduced = r.flag();
  bool decoder_model_info = false;
  int buffer_delay_length = 0;
  if (s.reduced) {
    s.level = static_cast<int>(r.bits(5));
  } else {
    if (r.flag()) {  // timing_info_present
      r.bits(32);
      r.bits(32);
      if (r.flag()) {
        int zeros = 0;
        while (!r.failed() && r.bits(1) == 0 && zeros < 32) ++zeros;
        if (zeros < 32) r.bits(zeros);
      }
      decoder_model_info = r.flag();
      if (decoder_model_info) {
        buffer_delay_length = static_cast<int>(r.bits(5)) + 1;
        r.bits(32);
        r.bits(5);
        r.bits(5);
      }
    }
    const bool initial_display_delay = r.flag();
    const int ops = static_cast<int>(r.bits(5)) + 1;
    for (int i = 0; i < ops; ++i) {
      r.bits(12);
      const int level = static_cast<int>(r.bits(5));
      const int tier = level > 7 ? static_cast<int>(r.bits(1)) : 0;
      if (i == 0) {
        s.level = level;
        s.tier = tier;
      }
      if (decoder_model_info && r.flag()) {
        r.bits(buffer_delay_length);
        r.bits(buffer_delay_length);
        r.bits(1);
      }
      if (initial_display_delay && r.flag()) r.bits(4);
    }
  }
  const int width_bits = static_cast<int>(r.bits(4)) + 1;
  const int height_bits = static_cast<int>(r.bits(4)) + 1;
  s.width = r.bits(width_bits) + 1;
  s.height = r.bits(height_bits) + 1;
  bool order_hint = false;
  if (!s.reduced && r.flag()) {
    r.bits(4);
    r.bits(3);
  }
  r.bits(3);  // use_128x128, filter_intra, intra_edge
  if (!s.reduced) {
    r.bits(4);  // interintra, masked, warped, dual_filter
    order_hint = r.flag();
    if (order_hint) r.bits(2);
    int force_screen = 2;
    if (!r.flag()) force_screen = static_cast<int>(r.bits(1));
    if (force_screen > 0 && !r.flag()) r.bits(1);
    if (order_hint) r.bits(3);
  }
  r.bits(3);  // superres, cdef, restoration
  s.high_bitdepth = r.flag();
  if (s.profile == 2 && s.high_bitdepth) s.twelve_bit = r.flag();
  s.mono = s.profile == 1 ? false : r.flag();
  int cp = 2;
  int tc = 2;
  int mc = 2;
  if (r.flag()) {
    cp = static_cast<int>(r.bits(8));
    tc = static_cast<int>(r.bits(8));
    mc = static_cast<int>(r.bits(8));
  }
  if (s.mono) {
    r.bits(1);
  } else if (cp == 1 && tc == 13 && mc == 0) {
    s.ss_x = 0;
    s.ss_y = 0;
  } else {
    r.bits(1);
    if (s.profile == 0) {
      s.ss_x = s.ss_y = 1;
    } else if (s.profile == 1) {
      s.ss_x = s.ss_y = 0;
    } else if (s.twelve_bit) {
      s.ss_x = static_cast<int>(r.bits(1));
      s.ss_y = s.ss_x ? static_cast<int>(r.bits(1)) : 0;
    } else {
      s.ss_x = 1;
      s.ss_y = 0;
    }
    if (s.ss_x && s.ss_y) s.sample_position = static_cast<int>(r.bits(2));
  }
  if (r.failed()) return std::nullopt;
  return s;
}

}  // namespace

std::optional<Bytes> build_av1c(ByteView obus) {
  for (const Obu& obu : split_obus(obus)) {
    if (obu.type != 1) continue;
    const auto s = parse_av1_sequence(obu.payload);
    if (!s) return std::nullopt;
    Bytes out;
    put_u8(out, 0x81);
    put_u8(out, static_cast<std::uint32_t>((s->profile << 5) | (s->level & 0x1F)));
    put_u8(out, static_cast<std::uint32_t>((s->tier << 7) | (s->high_bitdepth << 6) | (s->twelve_bit << 5) |
                                           (s->mono << 4) | (s->ss_x << 3) | (s->ss_y << 2) | s->sample_position));
    put_u8(out, 0);
    // configOBUs: the sequence header with a size field.
    const std::uint8_t header = std::to_integer<std::uint8_t>(obu.whole[0]);
    if ((header & 2) != 0) {
      out.insert(out.end(), obu.whole.begin(), obu.whole.end());
    } else {
      put_u8(out, header | 2u);
      std::size_t size = obu.payload.size();
      do {
        std::uint32_t b = size & 0x7F;
        size >>= 7;
        if (size != 0) b |= 0x80;
        put_u8(out, b);
      } while (size != 0);
      out.insert(out.end(), obu.payload.begin(), obu.payload.end());
    }
    return out;
  }
  return std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> av1_dimensions(ByteView obus) {
  for (const Obu& obu : split_obus(obus)) {
    if (obu.type != 1) continue;
    const auto s = parse_av1_sequence(obu.payload);
    if (!s) return std::nullopt;
    return std::pair{s->width, s->height};
  }
  return std::nullopt;
}

Bytes av1_strip_temporal_delimiters(ByteView obus) {
  Bytes out;
  const auto list = split_obus(obus);
  std::size_t consumed = 0;
  for (const Obu& obu : list) {
    consumed += obu.whole.size();
    if (obu.type == 2) continue;
    out.insert(out.end(), obu.whole.begin(), obu.whole.end());
  }
  if (consumed != obus.size()) return Bytes(obus.begin(), obus.end());  // not parseable: keep as is
  return out;
}

bool av1_is_keyframe(ByteView obus) noexcept {
  bool reduced = false;
  for (const Obu& obu : split_obus(obus)) {
    if (obu.type == 1) {
      const auto s = parse_av1_sequence(obu.payload);
      reduced = s && s->reduced;
      if (reduced) return true;
    }
    if (obu.type == 3 || obu.type == 6) {
      BitReader r(obu.payload);
      if (r.flag()) return false;  // show_existing_frame
      return r.bits(2) == 0;
    }
  }
  return false;
}

// --------------------------------------------------------------------------- VP8 / VP9

std::optional<Vp9Info> parse_vp9_frame(ByteView frame) noexcept {
  BitReader r(frame);
  Vp9Info info;
  if (r.bits(2) != 2) return std::nullopt;
  const int low = static_cast<int>(r.bits(1));
  const int high = static_cast<int>(r.bits(1));
  info.profile = (high << 1) | low;
  if (info.profile == 3) r.bits(1);
  if (r.flag()) return info;  // show_existing_frame
  info.keyframe = r.bits(1) == 0;
  r.bits(2);
  if (!info.keyframe) return r.failed() ? std::nullopt : std::optional<Vp9Info>(info);
  if (r.bits(24) != 0x498342) return std::nullopt;
  if (info.profile >= 2) info.bit_depth = r.flag() ? 12 : 10;
  const std::uint32_t color_space = r.bits(3);
  if (color_space != 7) {
    info.full_range = r.flag();
    if (info.profile == 1 || info.profile == 3) {
      const int sx = static_cast<int>(r.bits(1));
      const int sy = static_cast<int>(r.bits(1));
      r.bits(1);
      info.chroma_subsampling = (sx && sy) ? 1 : sx ? 2 : 3;
    }
  } else {
    info.full_range = true;
    info.chroma_subsampling = 3;
    if (info.profile == 1 || info.profile == 3) r.bits(1);
  }
  info.width = r.bits(16) + 1;
  info.height = r.bits(16) + 1;
  if (r.failed()) return std::nullopt;
  return info;
}

std::optional<Vp9Info> parse_vp8_frame(ByteView frame) noexcept {
  if (frame.size() < 3) return std::nullopt;
  Vp9Info info;
  info.keyframe = (at(frame, 0) & 1) == 0;
  if (info.keyframe) {
    if (frame.size() < 10 || at(frame, 3) != 0x9D || at(frame, 4) != 0x01 || at(frame, 5) != 0x2A) return std::nullopt;
    info.width = (at(frame, 6) | (static_cast<std::uint32_t>(at(frame, 7)) << 8)) & 0x3FFF;
    info.height = (at(frame, 8) | (static_cast<std::uint32_t>(at(frame, 9)) << 8)) & 0x3FFF;
  }
  return info;
}

Bytes build_vpcc(const Vp9Info& info) {
  Bytes out;
  put_u8(out, 1);  // FullBox version 1
  put_u8(out, 0);
  put_u8(out, 0);
  put_u8(out, 0);
  put_u8(out, static_cast<std::uint32_t>(info.profile));
  // The lowest level whose picture size and width/height limits fit (VP9
  // levels table of the WebM project; vpcC codes level X.Y as 10 * X + Y).
  // The frame rate is unknown here, so sample-rate limits are not applied.
  struct Level {
    int code;
    std::uint64_t picture;
    std::uint32_t breadth;
  };
  static constexpr Level levels[] = {{10, 36864, 512},     {11, 73728, 768},     {20, 122880, 960},
                                     {21, 245760, 1344},   {30, 552960, 2048},   {31, 983040, 2752},
                                     {40, 2228224, 4160},  {50, 8912896, 8384},  {60, 35651584, 16832}};
  const std::uint64_t samples = std::uint64_t{info.width} * info.height;
  int level = 0;  // undefined: larger than level 6.2
  for (const Level& l : levels) {
    if (samples <= l.picture && info.width <= l.breadth && info.height <= l.breadth) {
      level = l.code;
      break;
    }
  }
  put_u8(out, static_cast<std::uint32_t>(level));
  put_u8(out, static_cast<std::uint32_t>((info.bit_depth << 4) | (info.chroma_subsampling << 1) | (info.full_range ? 1 : 0)));
  put_u8(out, 2);
  put_u8(out, 2);
  put_u8(out, 2);
  put_be16(out, 0);
  return out;
}

// --------------------------------------------------------------------------- Opus

std::optional<Bytes> opus_head_to_dops(ByteView head) {
  util::ByteReader r(head);
  if (!r.consume("OpusHead")) return std::nullopt;
  r.skip(1);
  const std::uint8_t channels = r.u8();
  const std::uint16_t pre_skip = r.u16le();
  const std::uint32_t rate = r.u32le();
  const std::uint16_t gain = r.u16le();
  const std::uint8_t family = r.u8();
  if (!r.ok()) return std::nullopt;
  util::ByteWriter w;
  w.u8(0).u8(channels).u16be(pre_skip).u32be(rate).u16be(gain).u8(family);
  if (family != 0) {
    const std::uint8_t streams = r.u8();
    const std::uint8_t coupled = r.u8();
    const ByteView mapping = r.bytes(channels);
    if (!r.ok()) return std::nullopt;
    w.u8(streams).u8(coupled).bytes(mapping);
  }
  return w.take();
}

std::optional<Bytes> dops_to_opus_head(ByteView dops) {
  util::ByteReader r(dops);
  r.skip(1);
  const std::uint8_t channels = r.u8();
  const std::uint16_t pre_skip = r.u16be();
  const std::uint32_t rate = r.u32be();
  const std::uint16_t gain = r.u16be();
  const std::uint8_t family = r.u8();
  if (!r.ok()) return std::nullopt;
  util::ByteWriter w;
  w.text("OpusHead").u8(1).u8(channels).u16le(pre_skip).u32le(rate).u16le(gain).u8(family);
  if (family != 0) {
    const std::uint8_t streams = r.u8();
    const std::uint8_t coupled = r.u8();
    const ByteView mapping = r.bytes(channels);
    if (!r.ok()) return std::nullopt;
    w.u8(streams).u8(coupled).bytes(mapping);
  }
  return w.take();
}

Bytes make_opus_head(int channels, std::uint32_t input_rate, std::uint16_t pre_skip) {
  util::ByteWriter w;
  w.text("OpusHead").u8(1).u8(static_cast<std::uint8_t>(channels)).u16le(pre_skip).u32le(input_rate).u16le(0);
  if (channels <= 2) {
    w.u8(0);
  } else {
    // Vorbis channel order, one stream per channel (uncoupled): always valid.
    w.u8(1).u8(static_cast<std::uint8_t>(channels)).u8(0);
    for (int i = 0; i < channels; ++i) w.u8(static_cast<std::uint8_t>(i));
  }
  return w.take();
}

std::uint16_t opus_pre_skip(ByteView head) noexcept {
  if (head.size() < 12 || as_chars(head.first(8)) != "OpusHead") return 0;
  return static_cast<std::uint16_t>(at(head, 10) | (at(head, 11) << 8));
}

int opus_packet_samples(ByteView packet) noexcept {
  if (packet.empty()) return 0;
  const int toc = at(packet, 0);
  const int config = toc >> 3;
  int frame = 0;
  if (config < 12) {
    static constexpr std::array<int, 4> silk = {480, 960, 1920, 2880};
    frame = silk[static_cast<std::size_t>(config & 3)];
  } else if (config < 16) {
    frame = (config & 1) ? 960 : 480;
  } else {
    static constexpr std::array<int, 4> celt = {120, 240, 480, 960};
    frame = celt[static_cast<std::size_t>(config & 3)];
  }
  int count = 1;
  switch (toc & 3) {
    case 0: count = 1; break;
    case 1:
    case 2: count = 2; break;
    default:
      if (packet.size() < 2) return 0;
      count = at(packet, 1) & 0x3F;
  }
  return frame * count;
}

// --------------------------------------------------------------------------- FLAC

std::optional<FlacInfo> parse_flac_streaminfo(ByteView data) noexcept {
  util::ByteReader r(data);
  r.consume("fLaC");
  const std::uint8_t header = r.u8();
  const std::uint32_t length = r.u24be();
  if (!r.ok() || (header & 0x7F) != 0 || length < 34) return std::nullopt;
  const ByteView body = r.bytes(34);
  if (!r.ok()) return std::nullopt;
  BitReader b(body);
  FlacInfo info;
  b.bits(16);
  info.max_block = static_cast<int>(b.bits(16));
  b.bits(24);
  b.bits(24);
  info.sample_rate = b.bits(20);
  info.channels = static_cast<int>(b.bits(3)) + 1;
  info.bits = static_cast<int>(b.bits(5)) + 1;
  info.total_samples = (static_cast<std::uint64_t>(b.bits(4)) << 32) | b.bits(32);
  if (info.sample_rate == 0) return std::nullopt;
  return info;
}

// --------------------------------------------------------------------------- Xiph

std::optional<std::vector<Bytes>> split_xiph_lacing(ByteView data) {
  util::ByteReader r(data);
  const int count = r.u8() + 1;
  if (!r.ok() || count > 8) return std::nullopt;
  std::vector<std::uint64_t> sizes;
  for (int i = 0; i + 1 < count; ++i) {
    std::uint64_t size = 0;
    while (true) {
      const std::uint8_t b = r.u8();
      if (!r.ok()) return std::nullopt;
      size += b;
      if (b != 255) break;
    }
    sizes.push_back(size);
  }
  std::vector<Bytes> out;
  for (const std::uint64_t size : sizes) {
    if (size > r.remaining()) return std::nullopt;
    const ByteView part = r.bytes(static_cast<std::size_t>(size));
    out.emplace_back(part.begin(), part.end());
  }
  const ByteView last = r.rest();
  out.emplace_back(last.begin(), last.end());
  return out;
}

Bytes join_xiph_lacing(const std::vector<Bytes>& packets) {
  Bytes out;
  if (packets.empty()) return out;
  put_u8(out, static_cast<std::uint32_t>(packets.size() - 1));
  for (std::size_t i = 0; i + 1 < packets.size(); ++i) {
    std::size_t size = packets[i].size();
    while (size >= 255) {
      put_u8(out, 255);
      size -= 255;
    }
    put_u8(out, static_cast<std::uint32_t>(size));
  }
  for (const Bytes& p : packets) out.insert(out.end(), p.begin(), p.end());
  return out;
}

std::optional<VorbisInfo> parse_vorbis_id(ByteView packet) noexcept {
  util::ByteReader r(packet);
  if (r.u8() != 1 || !r.consume("vorbis")) return std::nullopt;
  r.skip(4);
  VorbisInfo info;
  info.channels = r.u8();
  info.sample_rate = r.u32le();
  r.skip(12);
  const std::uint8_t sizes = r.u8();
  if (!r.ok() || info.channels == 0 || info.sample_rate == 0) return std::nullopt;
  info.blocksize0 = 1 << (sizes & 0xF);
  info.blocksize1 = 1 << (sizes >> 4);
  return info;
}

bool VorbisDurations::init(ByteView identification, ByteView setup) {
  const auto id = parse_vorbis_id(identification);
  if (!id || setup.size() < 8) return false;
  bs0_ = id->blocksize0;
  bs1_ = id->blocksize1;
  const auto bit = [&](std::int64_t b) { return (std::to_integer<int>(setup[static_cast<std::size_t>(b >> 3)]) >> (b & 7)) & 1; };
  const auto field = [&](std::int64_t p, int n) {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<std::uint32_t>(bit(p + i)) << i;
    return v;
  };
  std::int64_t last = static_cast<std::int64_t>(setup.size()) * 8 - 1;
  while (last >= 0 && bit(last) == 0) --last;
  if (last < 0) return false;
  const std::int64_t framing = last;
  int candidates = 0;
  for (int m = 1; m <= 64; ++m) {
    const std::int64_t p = framing - 41LL * m;
    if (p < 7 * 8) break;
    if (field(p + 1, 16) != 0 || field(p + 17, 16) != 0) break;
    candidates = m;
  }
  for (int m = candidates; m >= 1; --m) {
    const std::int64_t count_at = framing - 41LL * m - 6;
    if (count_at < 7 * 8) continue;
    if (field(count_at, 6) == static_cast<std::uint32_t>(m - 1)) {
      modes_ = m;
      blockflag_.assign(static_cast<std::size_t>(m), false);
      for (int j = 0; j < m; ++j) blockflag_[static_cast<std::size_t>(j)] = bit(framing - 41LL * (m - j)) != 0;
      mode_bits_ = 0;
      while ((1 << mode_bits_) < m) ++mode_bits_;
      return true;
    }
  }
  return false;
}

int VorbisDurations::next(ByteView packet) {
  if (modes_ == 0 || packet.empty()) return -1;
  const int first = std::to_integer<int>(packet[0]);
  if ((first & 1) != 0) return -1;
  std::uint32_t mode = 0;
  for (int i = 0; i < mode_bits_; ++i) {
    const int b = 1 + i;
    if (static_cast<std::size_t>(b >> 3) >= packet.size()) return -1;
    mode |= static_cast<std::uint32_t>((std::to_integer<int>(packet[static_cast<std::size_t>(b >> 3)]) >> (b & 7)) & 1) << i;
  }
  if (mode >= static_cast<std::uint32_t>(modes_)) return -1;
  const int size = blockflag_[mode] ? bs1_ : bs0_;
  const int samples = (previous_ == 0 ? size : previous_) / 4 + size / 4;
  previous_ = size;
  return samples;
}

std::optional<TheoraInfo> parse_theora_id(ByteView packet) noexcept {
  util::ByteReader r(packet);
  if (r.u8() != 0x80 || !r.consume("theora")) return std::nullopt;
  r.skip(3);
  r.skip(2);
  r.skip(2);
  TheoraInfo info;
  info.width = r.u24be();
  info.height = r.u24be();
  r.skip(2);
  info.fps_num = r.u32be();
  info.fps_den = r.u32be();
  r.skip(6 + 1 + 3);
  const ByteView tail = r.bytes(2);
  if (!r.ok()) return std::nullopt;
  BitReader b(tail);
  b.bits(6);
  info.keyframe_shift = static_cast<int>(b.bits(5));
  return info;
}

std::optional<Mpeg12Info> parse_mpeg12_sequence(ByteView data) noexcept {
  for (std::size_t i = 0; i + 12 <= data.size(); ++i) {
    if (at(data, i) != 0 || at(data, i + 1) != 0 || at(data, i + 2) != 1 || at(data, i + 3) != 0xB3) continue;
    Mpeg12Info info;
    info.width = (static_cast<std::uint32_t>(at(data, i + 4)) << 4) | (at(data, i + 5) >> 4);
    info.height = ((static_cast<std::uint32_t>(at(data, i + 5)) & 0xF) << 8) | at(data, i + 6);
    const int code = at(data, i + 7) & 0xF;
    static constexpr std::array<std::pair<int, int>, 9> rates = {{{0, 1},
                                                                  {24000, 1001},
                                                                  {24, 1},
                                                                  {25, 1},
                                                                  {30000, 1001},
                                                                  {30, 1},
                                                                  {50, 1},
                                                                  {60000, 1001},
                                                                  {60, 1}}};
    if (code > 0 && code < 9) {
      info.fps_num = rates[static_cast<std::size_t>(code)].first;
      info.fps_den = rates[static_cast<std::size_t>(code)].second;
    }
    for (std::size_t k = i + 12; k + 10 <= data.size(); ++k) {
      if (at(data, k) == 0 && at(data, k + 1) == 0 && at(data, k + 2) == 1) {
        if (at(data, k + 3) == 0xB5 && (at(data, k + 4) >> 4) == 1) {
          info.mpeg2 = true;
          info.width |= static_cast<std::uint32_t>(((at(data, k + 5) & 1) << 1) | (at(data, k + 6) >> 7)) << 12;
          info.height |= static_cast<std::uint32_t>((at(data, k + 6) >> 5) & 3) << 12;
          const int ext_n = (at(data, k + 9) >> 5) & 3;
          const int ext_d = at(data, k + 9) & 0x1F;
          info.fps_num *= ext_n + 1;
          info.fps_den *= ext_d + 1;
        }
        if (at(data, k + 3) != 0xB5 && at(data, k + 3) != 0xB2) break;
      }
    }
    if (info.width == 0 || info.height == 0) return std::nullopt;
    return info;
  }
  return std::nullopt;
}

Bytes mpeg4_config(ByteView frame) {
  if (!starts_with_start_code(frame)) return {};
  for (std::size_t i = 0; i + 3 < frame.size(); ++i) {
    if (at(frame, i) == 0 && at(frame, i + 1) == 0 && at(frame, i + 2) == 1 && at(frame, i + 3) == 0xB6) {
      if (i == 0) return {};
      return Bytes(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }
  return {};
}

std::string fourcc_text(std::uint32_t value) {
  std::string s(4, '?');
  for (int i = 0; i < 4; ++i) {
    const auto c = static_cast<char>((value >> (24 - 8 * i)) & 0xFF);
    s[static_cast<std::size_t>(i)] = (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  return s;
}

}  // namespace cvt::video::bits

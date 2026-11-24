// Convertible core - video family: packaging conversions between containers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/video/adapt.hpp"

#include <utility>
#include <vector>

namespace cvt::video {

StreamAdapter::StreamAdapter(std::string codec, Bytes extradata, Packaging target)
    : codec_(std::move(codec)), original_extradata_(std::move(extradata)), target_(target) {
  if (is_nal_codec()) {
    const bool hevc = codec_ == "hevc";
    if (!original_extradata_.empty() && std::to_integer<int>(original_extradata_[0]) == 1) {
      const auto parsed = hevc ? bits::parse_hvcc(original_extradata_) : bits::parse_avcc(original_extradata_);
      if (parsed) {
        sets_ = *parsed;
        source_length_prefixed_ = true;
      }
    } else if (bits::starts_with_start_code(original_extradata_)) {
      bits::collect_parameter_sets(bits::split_annexb(original_extradata_), hevc, sets_);
    }
  } else if (codec_ == "aac" && !original_extradata_.empty()) {
    if (const auto config = bits::parse_asc(original_extradata_)) {
      aac_ = *config;
      has_aac_ = true;
    }
  }
}

void StreamAdapter::learn(ByteView packet) {
  if (is_nal_codec()) {
    const bool hevc = codec_ == "hevc";
    if (source_length_prefixed_) {
      if (const auto nals = bits::split_length_prefixed(packet, sets_.length_size)) {
        bits::collect_parameter_sets(*nals, hevc, sets_);
      }
    } else {
      bits::collect_parameter_sets(bits::split_annexb(packet), hevc, sets_);
    }
  } else if (codec_ == "aac") {
    if (!has_aac_) {
      if (const auto header = bits::parse_adts(packet)) {
        aac_ = header->config;
        has_aac_ = true;
      }
    }
  } else if (codec_ == "av1") {
    if (learned_.empty()) {
      if (auto config = bits::build_av1c(packet)) learned_ = std::move(*config);
    }
  } else if (codec_ == "mpeg4") {
    if (learned_.empty() && original_extradata_.empty()) learned_ = bits::mpeg4_config(packet);
  }
}

Result<Bytes> StreamAdapter::convert(ByteView packet, bool keyframe) {
  learn(packet);
  if (is_nal_codec()) {
    const bool hevc = codec_ == "hevc";
    std::vector<ByteView> nals;
    if (source_length_prefixed_) {
      if (target_.length_prefixed && sets_.length_size == 4) return Bytes(packet.begin(), packet.end());
      auto split = bits::split_length_prefixed(packet, sets_.length_size);
      if (!split) return fail(Status::Decode, "{} packet has a NAL unit length that overruns the packet", codec_);
      nals = std::move(*split);
    } else {
      nals = bits::split_annexb(packet);
      if (nals.empty() && !packet.empty()) {
        return fail(Status::Decode, "{} packet is neither Annex-B nor length-prefixed", codec_);
      }
    }
    if (target_.length_prefixed) return bits::to_length_prefixed(nals);
    std::vector<ByteView> out;
    const auto aud_type = hevc ? 35 : 9;
    const bool has_aud = !nals.empty() && (hevc ? bits::hevc_nal_type(nals[0]) : bits::h264_nal_type(nals[0])) == aud_type;
    static constexpr std::byte h264_aud[] = {std::byte{0x09}, std::byte{0xF0}};
    static constexpr std::byte hevc_aud[] = {std::byte{0x46}, std::byte{0x01}, std::byte{0x50}};
    if (target_.access_unit_delimiters && !has_aud) {
      out.push_back(hevc ? ByteView(hevc_aud) : ByteView(h264_aud));
    }
    std::size_t first = 0;
    if (has_aud) {
      out.push_back(nals[0]);
      first = 1;
    }
    bool has_sps = false;
    for (const ByteView nal : nals) {
      const int type = hevc ? bits::hevc_nal_type(nal) : bits::h264_nal_type(nal);
      if (type == (hevc ? 33 : 7)) has_sps = true;
    }
    if (keyframe && !has_sps) {
      for (const Bytes& p : sets_.vps) out.emplace_back(p);
      for (const Bytes& p : sets_.sps) out.emplace_back(p);
      for (const Bytes& p : sets_.pps) out.emplace_back(p);
    }
    for (std::size_t i = first; i < nals.size(); ++i) out.push_back(nals[i]);
    return bits::to_annexb(out);
  }
  if (codec_ == "aac") {
    const auto header = bits::parse_adts(packet);
    const bool is_adts = header && header->frame_size <= packet.size() && original_extradata_.empty();
    if (target_.adts) {
      if (is_adts) return Bytes(packet.begin(), packet.end());
      if (!has_aac_) return fail(Status::Encode, "the AAC configuration is unknown, so ADTS headers cannot be written");
      Bytes out = bits::build_adts_header(aac_, packet.size());
      out.insert(out.end(), packet.begin(), packet.end());
      return out;
    }
    if (is_adts) {
      return Bytes(packet.begin() + static_cast<std::ptrdiff_t>(header->header_size),
                   packet.begin() + static_cast<std::ptrdiff_t>(header->frame_size));
    }
    return Bytes(packet.begin(), packet.end());
  }
  if (codec_ == "av1") {
    if (target_.av1_temporal_delimiters) {
      if (!packet.empty() && ((std::to_integer<int>(packet[0]) >> 3) & 0xF) == 2) return Bytes(packet.begin(), packet.end());
      Bytes out = {std::byte{0x12}, std::byte{0x00}};
      out.insert(out.end(), packet.begin(), packet.end());
      return out;
    }
    return bits::av1_strip_temporal_delimiters(packet);
  }
  return Bytes(packet.begin(), packet.end());
}

Bytes StreamAdapter::extradata() const {
  if (is_nal_codec()) {
    if (!target_.length_prefixed) return {};
    if (source_length_prefixed_ && !original_extradata_.empty()) return original_extradata_;
    const bool hevc = codec_ == "hevc";
    if (!sets_.complete(hevc)) return {};
    auto built = hevc ? bits::build_hvcc(sets_) : bits::build_avcc(sets_);
    return built ? std::move(*built) : Bytes{};
  }
  if (codec_ == "aac") {
    if (target_.adts) return {};
    if (!original_extradata_.empty()) return original_extradata_;
    return has_aac_ ? bits::build_asc(aac_) : Bytes{};
  }
  if (codec_ == "av1") {
    if (target_.av1_temporal_delimiters) return original_extradata_;
    return !original_extradata_.empty() ? original_extradata_ : learned_;
  }
  if (codec_ == "mpeg4") return !original_extradata_.empty() ? original_extradata_ : learned_;
  return original_extradata_;
}

bool StreamAdapter::configured() const {
  if (is_nal_codec()) return !target_.length_prefixed || !extradata().empty();
  if (codec_ == "aac") return has_aac_ || !original_extradata_.empty();
  return true;
}

}  // namespace cvt::video

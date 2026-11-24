// Convertible core - video family: which codecs each container can hold and
// which ones the converter picks when it has to encode.
//
// Two lists per container and media type:
//   storable   every codec the muxer can tag, so a stream in one of them is
//              copied unchanged (remux) when no option asks for re-encoding;
//   preferred  the codecs tried, in order, when encoding: the first one that
//              has an encoder registered in codec::Registry wins. The first
//              entry is the container's default (mp4: h264 + aac, webm:
//              vp9 + opus, ogv: theora + vorbis, ...).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codec/registry.hpp"
#include "engine/errors.hpp"
#include "ir/video.hpp"

namespace cvt::video {

struct ContainerPolicy {
  std::string_view format_id;
  std::vector<std::string_view> video_storable;
  std::vector<std::string_view> audio_storable;
  std::vector<std::string_view> video_preferred;
  std::vector<std::string_view> audio_preferred;
  /// Stores display rotation as metadata (otherwise frames are rotated when re-encoding).
  bool stores_rotation = false;
  /// Needs a constant frame rate (frames are duplicated/dropped to the target rate).
  bool constant_frame_rate = false;
  /// Can hold more than one audio track.
  bool multiple_audio = true;
};

/// The policy for a container format id, or nullptr.
[[nodiscard]] const ContainerPolicy* policy_for(std::string_view format_id);
/// All container ids with a muxer.
[[nodiscard]] std::span<const ContainerPolicy> all_policies();

/// Result of picking an encoder.
struct CodecChoice {
  std::string codec;
};

/// Pick the codec for one media type: `requested` is the user's option
/// ("auto" or a codec id); the preference list is walked down to the first
/// codec with a registered encoder. Errors name the container, the codecs it
/// accepts and what this build offers.
[[nodiscard]] Result<CodecChoice> choose_encoder(const ContainerPolicy& policy, ir::TrackType type,
                                                 std::string_view requested, const codec::Registry& media);

/// Every codec id that can appear in the `video_codec` / `audio_codec` options.
[[nodiscard]] std::vector<std::string> video_codec_choices();
[[nodiscard]] std::vector<std::string> audio_codec_choices();

}  // namespace cvt::video

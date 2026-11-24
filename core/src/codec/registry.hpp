// Convertible core - the codec registry: decoder and encoder factories by
// canonical codec id.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codec/codec.hpp"

namespace cvt::codec {

using VideoDecoderFactory = std::function<std::unique_ptr<VideoDecoder>()>;
using VideoEncoderFactory = std::function<std::unique_ptr<VideoEncoder>()>;
using AudioDecoderFactory = std::function<std::unique_ptr<AudioDecoder>()>;
using AudioEncoderFactory = std::function<std::unique_ptr<AudioEncoder>()>;

enum class CodecRole { VideoDecoder, VideoEncoder, AudioDecoder, AudioEncoder };

[[nodiscard]] constexpr std::string_view codec_role_name(CodecRole role) noexcept {
  switch (role) {
    case CodecRole::VideoDecoder: return "video decoder";
    case CodecRole::VideoEncoder: return "video encoder";
    case CodecRole::AudioDecoder: return "audio decoder";
    case CodecRole::AudioEncoder: return "audio encoder";
  }
  return "codec";
}

/// One registered implementation, for diagnostics.
struct CodecListing {
  std::string id;
  CodecRole role = CodecRole::VideoDecoder;
};

/// Factories for decoders and encoders, keyed by canonical codec id
/// (codec/ids.hpp).
///
/// One instance lives inside the format registry (`cvt::Registry::media()`),
/// is filled by the families during registration and is immutable
/// afterwards, so lookups are thread-safe. Each factory call returns a fresh,
/// independent object.
///
///   // in register_codec_h264_family(Registry& registry):
///   registry.media().register_video_decoder(codec::id::h264, [] { return std::make_unique<H264Decoder>(); });
///
///   // in a demuxer:
///   auto decoder = ctx.registry->media().make_video_decoder(track.codec);
///   if (!decoder) return fail(Status::Decode, "no decoder for {}", track.codec);
class Registry {
 public:
  /// Register THE implementation of an (id, role) pair, as the family that
  /// owns the codec. Ownership rule (deterministic, independent of the order
  /// in which families register):
  ///   - one owner per (id, role): a second owner registration is ignored,
  ///     reported by `problems()` (and so by `cvt::Registry::validate()`),
  ///     and the first one stays;
  ///   - an owner registration replaces a fallback registered earlier.
  /// Returns false when ignored.
  bool register_video_decoder(std::string_view id, VideoDecoderFactory factory);
  bool register_video_encoder(std::string_view id, VideoEncoderFactory factory);
  bool register_audio_decoder(std::string_view id, AudioDecoderFactory factory);
  bool register_audio_encoder(std::string_view id, AudioEncoderFactory factory);

  /// Register a FALLBACK implementation, used only while no owner registers
  /// one (e.g. the video family's simple PCM codecs, which stand in when the
  /// lossless-audio family is absent from a build). Silently ignored when an
  /// implementation (owner or fallback) is already registered; silently
  /// replaced when an owner registers later. Returns false when ignored.
  bool register_video_decoder_fallback(std::string_view id, VideoDecoderFactory factory);
  bool register_video_encoder_fallback(std::string_view id, VideoEncoderFactory factory);
  bool register_audio_decoder_fallback(std::string_view id, AudioDecoderFactory factory);
  bool register_audio_encoder_fallback(std::string_view id, AudioEncoderFactory factory);

  /// True when the implementation registered for (id, role) is a fallback.
  [[nodiscard]] bool is_fallback(std::string_view id, CodecRole role) const;

  /// The factory registered for `id`, or nullopt.
  [[nodiscard]] std::optional<VideoDecoderFactory> video_decoder(std::string_view id) const;
  [[nodiscard]] std::optional<VideoEncoderFactory> video_encoder(std::string_view id) const;
  [[nodiscard]] std::optional<AudioDecoderFactory> audio_decoder(std::string_view id) const;
  [[nodiscard]] std::optional<AudioEncoderFactory> audio_encoder(std::string_view id) const;

  /// Convenience: a new instance, or nullptr when no implementation exists.
  [[nodiscard]] std::unique_ptr<VideoDecoder> make_video_decoder(std::string_view id) const;
  [[nodiscard]] std::unique_ptr<VideoEncoder> make_video_encoder(std::string_view id) const;
  [[nodiscard]] std::unique_ptr<AudioDecoder> make_audio_decoder(std::string_view id) const;
  [[nodiscard]] std::unique_ptr<AudioEncoder> make_audio_encoder(std::string_view id) const;

  [[nodiscard]] bool has_video_decoder(std::string_view id) const { return video_decoders_.contains(id); }
  [[nodiscard]] bool has_video_encoder(std::string_view id) const { return video_encoders_.contains(id); }
  [[nodiscard]] bool has_audio_decoder(std::string_view id) const { return audio_decoders_.contains(id); }
  [[nodiscard]] bool has_audio_encoder(std::string_view id) const { return audio_encoders_.contains(id); }

  /// Everything registered, sorted by role and id.
  [[nodiscard]] std::vector<CodecListing> list() const;

  /// Registration mistakes: unknown (non-canonical) ids, empty factories,
  /// duplicate registrations. Empty when all is well.
  [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }

 private:
  template <class Factory>
  struct Entry {
    Factory factory;
    bool fallback = false;
  };
  template <class Factory>
  using Table = std::map<std::string, Entry<Factory>, std::less<>>;
  template <class Factory>
  bool add(Table<Factory>& table, CodecRole role, std::string_view id, Factory factory, bool fallback);

  Table<VideoDecoderFactory> video_decoders_;
  Table<VideoEncoderFactory> video_encoders_;
  Table<AudioDecoderFactory> audio_decoders_;
  Table<AudioEncoderFactory> audio_encoders_;
  std::vector<std::string> problems_;
};

}  // namespace cvt::codec

// Convertible core - audio_lossy family: AAC (decoder, encoder, ADTS and
// MPEG-4 file handling).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codec/codec.hpp"
#include "engine/context.hpp"
#include "engine/errors.hpp"
#include "ir/audio.hpp"
#include "util/bytes.hpp"

namespace cvt::lossy::aac {

/// The fields of an MPEG-4 AudioSpecificConfig that matter here.
struct AudioConfig {
  int object_type = 2;  ///< 2 = AAC-LC, 5 = SBR (HE-AAC), 29 = PS (HE-AAC v2)
  std::uint32_t sample_rate = 0;
  int channel_config = 0;
  bool sbr = false;
  bool ps = false;
  std::uint32_t extension_rate = 0;  ///< output rate with SBR
};
[[nodiscard]] std::optional<AudioConfig> parse_audio_config(ByteView asc);
/// AAC-LC AudioSpecificConfig (2 bytes, more for unusual rates).
[[nodiscard]] Bytes make_lc_config(std::uint32_t sample_rate, int channels);
/// Index of `rate` in the MPEG-4 sampling frequency table, or 15.
[[nodiscard]] int rate_index(std::uint32_t rate) noexcept;
[[nodiscard]] std::uint32_t rate_from_index(int index) noexcept;

/// Decodes raw AAC access units (one per call) into WAVE-ordered floats.
class Decoder {
 public:
  Decoder();
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  [[nodiscard]] Result<void> configure(ByteView audio_specific_config);
  /// Appends the samples of one access unit; returns the sample frames.
  [[nodiscard]] Result<std::uint32_t> decode(ByteView access_unit, std::vector<float>& out);
  [[nodiscard]] std::uint32_t sample_rate() const noexcept;
  [[nodiscard]] int channels() const noexcept;
  /// Extra output delay of this decoder (its SBR filterbanks) that encoders'
  /// priming values do not include; 0 for plain AAC-LC.
  [[nodiscard]] std::uint32_t extra_delay() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// AAC-LC encoder: 1024 sample frames in, raw access units out.
class Encoder {
 public:
  Encoder();
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  [[nodiscard]] Result<void> configure(std::uint32_t sample_rate, int channels, std::uint32_t bitrate);
  /// One frame of 1024 interleaved sample frames.
  [[nodiscard]] Result<Bytes> encode(const float* samples);
  [[nodiscard]] Bytes config() const;
  /// Priming samples at the start of the decoded stream.
  [[nodiscard]] static constexpr std::uint32_t delay() noexcept { return 1600; }  // one frame plus the encoder look-ahead (measured)

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// A decoded track and its gapless information.
struct Track {
  Bytes config;
  std::vector<ByteView> units;
  std::uint64_t priming = 0;
  std::uint64_t valid = 0;  ///< 0 = everything
  std::map<std::string, std::string> tags;
};

[[nodiscard]] Result<ir::Audio> decode_track(const Track& track, const Context& ctx);

/// ADTS stream -> track (no gapless information in ADTS).
[[nodiscard]] Result<Track> read_adts(ByteView data, const Context& ctx);
[[nodiscard]] Bytes adts_header(const AudioConfig& config, std::size_t payload);

/// Encode audio as AAC access units.
struct Encoded {
  std::vector<Bytes> units;
  Bytes config;
  std::uint64_t priming = 0;
  std::uint64_t frames = 0;  ///< valid sample frames
  std::uint32_t sample_rate = 0;
  int channels = 0;
};
[[nodiscard]] Result<Encoded> encode_audio(const ir::Audio& audio, std::uint32_t bitrate, const Context& ctx);

/// MPEG-4 audio file with an AAC track; nullopt when the file holds another
/// codec (the caller then uses the generic reader).
[[nodiscard]] Result<std::optional<Track>> read_mp4_aac(ByteView data, const Context& ctx);
/// A complete .m4a file (moov before mdat, edit list + iTunSMPB for gapless).
[[nodiscard]] Result<Bytes> write_m4a(const Encoded& stream, const std::map<std::string, std::string>& tags,
                                      std::string_view brand, const Context& ctx);

[[nodiscard]] std::unique_ptr<codec::AudioDecoder> make_aac_decoder();
[[nodiscard]] std::unique_ptr<codec::AudioEncoder> make_aac_encoder();

}  // namespace cvt::lossy::aac

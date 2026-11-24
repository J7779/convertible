// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/registry.hpp"

#include <format>
#include <utility>

#include "codec/ids.hpp"

namespace cvt::codec {

namespace {

template <class Table>
auto lookup(const Table& table, std::string_view id) -> std::optional<typename Table::mapped_type> {
  const auto it = table.find(id);
  if (it == table.end()) return std::nullopt;
  return it->second;
}

template <class Table>
auto make(const Table& table, std::string_view id) -> decltype(table.begin()->second()) {
  const auto it = table.find(id);
  if (it == table.end()) return nullptr;
  return it->second();
}

}  // namespace

template <class Factory>
bool Registry::add(Table<Factory>& table, CodecRole role, std::string_view id, Factory factory) {
  if (!id::is_known(id)) {
    problems_.push_back(std::format("{} '{}': not a canonical codec id (see codec/ids.hpp)", codec_role_name(role), id));
  }
  if (!factory) {
    problems_.push_back(std::format("{} '{}': empty factory", codec_role_name(role), id));
    return false;
  }
  const auto [it, inserted] = table.emplace(std::string(id), std::move(factory));
  if (!inserted) problems_.push_back(std::format("{} '{}': registered twice", codec_role_name(role), id));
  return inserted;
}

bool Registry::register_video_decoder(std::string_view id, VideoDecoderFactory factory) {
  return add(video_decoders_, CodecRole::VideoDecoder, id, std::move(factory));
}
bool Registry::register_video_encoder(std::string_view id, VideoEncoderFactory factory) {
  return add(video_encoders_, CodecRole::VideoEncoder, id, std::move(factory));
}
bool Registry::register_audio_decoder(std::string_view id, AudioDecoderFactory factory) {
  return add(audio_decoders_, CodecRole::AudioDecoder, id, std::move(factory));
}
bool Registry::register_audio_encoder(std::string_view id, AudioEncoderFactory factory) {
  return add(audio_encoders_, CodecRole::AudioEncoder, id, std::move(factory));
}

std::optional<VideoDecoderFactory> Registry::video_decoder(std::string_view id) const { return lookup(video_decoders_, id); }
std::optional<VideoEncoderFactory> Registry::video_encoder(std::string_view id) const { return lookup(video_encoders_, id); }
std::optional<AudioDecoderFactory> Registry::audio_decoder(std::string_view id) const { return lookup(audio_decoders_, id); }
std::optional<AudioEncoderFactory> Registry::audio_encoder(std::string_view id) const { return lookup(audio_encoders_, id); }

std::unique_ptr<VideoDecoder> Registry::make_video_decoder(std::string_view id) const { return make(video_decoders_, id); }
std::unique_ptr<VideoEncoder> Registry::make_video_encoder(std::string_view id) const { return make(video_encoders_, id); }
std::unique_ptr<AudioDecoder> Registry::make_audio_decoder(std::string_view id) const { return make(audio_decoders_, id); }
std::unique_ptr<AudioEncoder> Registry::make_audio_encoder(std::string_view id) const { return make(audio_encoders_, id); }

std::vector<CodecListing> Registry::list() const {
  std::vector<CodecListing> out;
  for (const auto& [id, factory] : video_decoders_) out.push_back({id, CodecRole::VideoDecoder});
  for (const auto& [id, factory] : video_encoders_) out.push_back({id, CodecRole::VideoEncoder});
  for (const auto& [id, factory] : audio_decoders_) out.push_back({id, CodecRole::AudioDecoder});
  for (const auto& [id, factory] : audio_encoders_) out.push_back({id, CodecRole::AudioEncoder});
  return out;
}

}  // namespace cvt::codec

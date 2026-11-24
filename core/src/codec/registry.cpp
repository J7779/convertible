// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "codec/registry.hpp"

#include <format>
#include <utility>

#include "codec/ids.hpp"

namespace cvt::codec {

namespace {

template <class Table>
auto lookup(const Table& table, std::string_view id) -> std::optional<decltype(table.begin()->second.factory)> {
  const auto it = table.find(id);
  if (it == table.end()) return std::nullopt;
  return it->second.factory;
}

template <class Table>
auto make(const Table& table, std::string_view id) -> decltype(table.begin()->second.factory()) {
  const auto it = table.find(id);
  if (it == table.end()) return nullptr;
  return it->second.factory();
}

template <class Table>
bool fallback_in(const Table& table, std::string_view id) {
  const auto it = table.find(id);
  return it != table.end() && it->second.fallback;
}

}  // namespace

template <class Factory>
bool Registry::add(Table<Factory>& table, CodecRole role, std::string_view id, Factory factory, bool fallback) {
  if (!id::is_known(id)) {
    problems_.push_back(std::format("{} '{}': not a canonical codec id (see codec/ids.hpp)", codec_role_name(role), id));
  }
  if (!factory) {
    problems_.push_back(std::format("{} '{}': empty factory", codec_role_name(role), id));
    return false;
  }
  const auto it = table.find(id);
  if (it == table.end()) {
    table.emplace(std::string(id), Entry<Factory>{std::move(factory), fallback});
    return true;
  }
  if (fallback) return false;  // an implementation exists: the fallback is not needed
  if (it->second.fallback) {   // the owner replaces a fallback
    it->second = Entry<Factory>{std::move(factory), false};
    return true;
  }
  problems_.push_back(std::format("{} '{}': registered twice (one family must own it; others may only register a fallback)",
                                  codec_role_name(role), id));
  return false;
}

bool Registry::register_video_decoder(std::string_view id, VideoDecoderFactory factory) {
  return add(video_decoders_, CodecRole::VideoDecoder, id, std::move(factory), false);
}
bool Registry::register_video_encoder(std::string_view id, VideoEncoderFactory factory) {
  return add(video_encoders_, CodecRole::VideoEncoder, id, std::move(factory), false);
}
bool Registry::register_audio_decoder(std::string_view id, AudioDecoderFactory factory) {
  return add(audio_decoders_, CodecRole::AudioDecoder, id, std::move(factory), false);
}
bool Registry::register_audio_encoder(std::string_view id, AudioEncoderFactory factory) {
  return add(audio_encoders_, CodecRole::AudioEncoder, id, std::move(factory), false);
}
bool Registry::register_video_decoder_fallback(std::string_view id, VideoDecoderFactory factory) {
  return add(video_decoders_, CodecRole::VideoDecoder, id, std::move(factory), true);
}
bool Registry::register_video_encoder_fallback(std::string_view id, VideoEncoderFactory factory) {
  return add(video_encoders_, CodecRole::VideoEncoder, id, std::move(factory), true);
}
bool Registry::register_audio_decoder_fallback(std::string_view id, AudioDecoderFactory factory) {
  return add(audio_decoders_, CodecRole::AudioDecoder, id, std::move(factory), true);
}
bool Registry::register_audio_encoder_fallback(std::string_view id, AudioEncoderFactory factory) {
  return add(audio_encoders_, CodecRole::AudioEncoder, id, std::move(factory), true);
}

bool Registry::is_fallback(std::string_view id, CodecRole role) const {
  switch (role) {
    case CodecRole::VideoDecoder: return fallback_in(video_decoders_, id);
    case CodecRole::VideoEncoder: return fallback_in(video_encoders_, id);
    case CodecRole::AudioDecoder: return fallback_in(audio_decoders_, id);
    case CodecRole::AudioEncoder: return fallback_in(audio_encoders_, id);
  }
  return false;
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
  for (const auto& [id, entry] : video_decoders_) out.push_back({id, CodecRole::VideoDecoder});
  for (const auto& [id, entry] : video_encoders_) out.push_back({id, CodecRole::VideoEncoder});
  for (const auto& [id, entry] : audio_decoders_) out.push_back({id, CodecRole::AudioDecoder});
  for (const auto& [id, entry] : audio_encoders_) out.push_back({id, CodecRole::AudioEncoder});
  return out;
}

}  // namespace cvt::codec

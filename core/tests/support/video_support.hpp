// Helpers for tests of anything that produces or consumes ir::Video: drain a
// video into frames and samples, collect a track's packets, and read a file
// while keeping its bytes alive for the streaming IR.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "support/test_support.hpp"

#include "ir/video.hpp"

namespace cvt::test {

/// Everything a Video yields when decoded completely.
struct Drained {
  std::vector<ir::VideoFrame> frames;
  std::vector<std::int64_t> audio_pts_ms;
  std::vector<float> samples;
};

/// Pull every frame (up to `max_frames`) and every audio chunk out of `video`.
inline Result<Drained> drain(ir::Video& video, std::size_t max_frames = 10000) {
  Drained out;
  if (video.next_frame) {
    while (out.frames.size() < max_frames) {
      CVT_TRY_ASSIGN(auto frame, video.next_frame());
      if (!frame) break;
      out.frames.push_back(std::move(*frame));
    }
  }
  if (video.next_audio) {
    while (true) {
      CVT_TRY_ASSIGN(auto chunk, video.next_audio());
      if (!chunk) break;
      out.audio_pts_ms.push_back(chunk->pts_ms);
      out.samples.insert(out.samples.end(), chunk->samples.begin(), chunk->samples.end());
    }
  }
  return out;
}

/// Every packet of a track.
inline Result<std::vector<ir::Packet>> packets_of(const ir::Track& track) {
  std::vector<ir::Packet> out;
  if (!track.next_packet) return out;
  while (true) {
    CVT_TRY_ASSIGN(auto p, track.next_packet());
    if (!p) break;
    out.push_back(std::move(*p));
  }
  return out;
}

/// A decoded value together with the input bytes it may still refer to.
///
/// PITFALL: readers of streaming IRs (ir::Video, and any IR that keeps views)
/// refer to the input bytes until the value is consumed; the engine keeps the
/// input alive until the writer returns. In a test,
///   auto video = test::read_as<ir::Video>("mp4", test::fixture("video/a.mp4"));
/// reads from a temporary that is gone by the next statement. Keep the bytes
/// in a named variable, or use `read_owned`, which keeps them alive here.
template <IrType T>
struct Owned {
  std::shared_ptr<const Bytes> input;
  T value;
  T* operator->() noexcept { return &value; }
  T& operator*() noexcept { return value; }
};

/// `read_as`, keeping `input` alive for as long as the result lives.
template <IrType T>
[[nodiscard]] Result<Owned<T>> read_owned(std::string_view format_id, Bytes input, std::string_view options_json = {},
                                          std::vector<std::string>* warnings = nullptr, const Limits& limits = {}) {
  auto kept = std::make_shared<const Bytes>(std::move(input));
  CVT_TRY_ASSIGN(T value, read_as<T>(format_id, *kept, options_json, warnings, limits));
  return Owned<T>{std::move(kept), std::move(value)};
}

}  // namespace cvt::test

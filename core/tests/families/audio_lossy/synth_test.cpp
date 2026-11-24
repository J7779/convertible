// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>

#include "families/audio_lossy/lossy_test_util.hpp"
#include "ir/data.hpp"
#include "ir/table.hpp"

using namespace cvt;

namespace {

/// Frequency of the strongest component between `from` and `from + n`
/// frames (mono mix), by a fine DFT scan around the coarse maximum.
double pitch(const ir::Audio& a, std::size_t from, std::size_t n) {
  std::vector<double> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    double v = 0;
    for (std::size_t c = 0; c < a.channels; ++c) v += a.samples[(from + i) * a.channels + c];
    x[i] = v * (0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n)));
  }
  const auto power = [&](double f) {
    std::complex<double> s;
    for (std::size_t i = 0; i < n; ++i) s += x[i] * std::polar(1.0, -2 * std::numbers::pi * f * static_cast<double>(i) / a.sample_rate);
    return std::norm(s);
  };
  double best = 0, best_f = 0;
  for (double f = 50; f < 2000; f += 2) {
    const double p = power(f);
    if (p > best) best = p, best_f = f;
  }
  for (double f = best_f - 2; f <= best_f + 2; f += 0.05) {
    const double p = power(f);
    if (p > best) best = p, best_f = f;
  }
  return best_f;
}

double last_sound(const ir::Audio& a) {
  std::size_t last = 0;
  for (std::size_t i = 0; i < a.samples.size(); ++i) {
    if (std::abs(a.samples[i]) > 1e-4f) last = i / a.channels;
  }
  return static_cast<double>(last) / a.sample_rate;
}

}  // namespace

TEST_CASE("audio_lossy/mid: event lists match an independent parser (mido)") {
  const Bytes data = test::fixture("audio_lossy/synth/mido_type1.mid");
  const auto json = test::read_as<ir::Data>("mid", data);
  REQUIRE_OK(json);
  // Values printed by mido for this file: 36 messages, 13 note_on (mido
  // counts note_on with velocity 0 as note_on; this file has none), length 4.1 s.
  const Json& events = json->value["events"];
  CHECK(events.size() == 36);
  std::size_t note_on = 0;
  for (const Json& e : events.elements()) note_on += e["type"].as_string() == "note_on" ? 1 : 0;
  CHECK(note_on == 13);
  CHECK_APPROX(json->value["duration_seconds"].as_double(), 4.1, 1e-9);
  CHECK(json->value["tracks"].as_int64() == 3);
  const auto table = test::read_as<ir::Table>("mid", data);
  REQUIRE_OK(table);
  CHECK(table->sheets[0].rows.size() == 37);
}

TEST_CASE("audio_lossy/mid: renders the right notes at the right times") {
  const auto a4 = test::read_as<ir::Audio>("mid", test::fixture("audio_lossy/synth/mido_type0_a4.mid"));
  REQUIRE_OK(a4);
  CHECK(a4->sample_rate == 44100);
  CHECK(a4->channels == 2);
  CHECK_APPROX(pitch(*a4, 22050, 8192), 440.0, 1.5);
  CHECK(last_sound(*a4) > 1.9);
  CHECK(last_sound(*a4) < 2.6);  // two seconds plus the release
  const auto song = test::read_as<ir::Audio>("mid", test::fixture("audio_lossy/synth/mido_type1.mid"));
  REQUIRE_OK(song);
  CHECK(song->tags.at("title") == "Test Song");
  CHECK_APPROX(pitch(*song, 2000, 8192), 440.0, 1.5);  // first piano note A4 (with the drums)
  CHECK(last_sound(*song) > 4.0);
  CHECK(last_sound(*song) < 6.2);
  const auto fast = test::read_as<ir::Audio>("mid", test::fixture("audio_lossy/synth/mido_type0_a4.mid"),
                                             R"({"render_sample_rate":22050})");
  REQUIRE_OK(fast);
  CHECK(fast->sample_rate == 22050);
}

TEST_CASE("audio_lossy/tracker: modules play like an independent player (libopenmpt)") {
  struct Case {
    const char* format;
    const char* name;
    double pitch1, pitch2, end;
  };
  // Pitches and lengths measured on openmpt123's renders of the same files.
  for (const Case& c : {Case{"mod", "sine_4ch.mod", 258.8, 436.3, 2.02}, Case{"s3m", "sine.s3m", 261.5, 440.6, 2.02},
                        Case{"xm", "sine_env.xm", 130.4, 130.4, 1.56}, Case{"it", "sine.it", 261.5, 439.8, 2.02}}) {
    INFO(c.name);
    const auto audio = test::read_as<ir::Audio>(c.format, test::fixture(std::string("audio_lossy/synth/") + c.name));
    REQUIRE_OK(audio);
    const double p1 = pitch(*audio, 1000, 8192);
    const double p2 = std::string(c.format) == "xm" ? p1 : pitch(*audio, 43218, 8192);
    std::printf("%s: %.1f Hz, %.1f Hz, last sound %.3f s, length %.3f s\n", c.name, p1, p2, last_sound(*audio),
                audio->duration_seconds());
    CHECK_APPROX(p1, c.pitch1, c.pitch1 * 0.006);
    CHECK_APPROX(p2, c.pitch2, c.pitch2 * 0.006);
    CHECK_APPROX(last_sound(*audio), c.end, 0.15);
  }
}

TEST_CASE("audio_lossy/synth: hostile input never crashes") {
  for (const char* f : {"mido_type1.mid", "sine_4ch.mod", "sine.s3m", "sine_env.xm", "sine.it"}) {
    const std::string name(f);
    const std::string format = name.substr(name.rfind('.') + 1);
    const Bytes data = test::fixture("audio_lossy/synth/" + name);
    // Every truncation and a set of single-byte corruptions, rendering at
    // most two seconds each.
    std::size_t runs = 0;
    for (std::size_t n = 0; n < data.size(); n += std::max<std::size_t>(1, data.size() / 97)) {
      (void)test::read_as<ir::Audio>(format, ByteView(data).first(n), R"({"render_max_seconds":2})");
      ++runs;
    }
    for (std::size_t i = 0; i < data.size(); i += std::max<std::size_t>(1, data.size() / 61)) {
      Bytes broken = data;
      broken[i] = static_cast<std::byte>(std::to_integer<int>(broken[i]) ^ 0xA5);
      (void)test::read_as<ir::Audio>(format, broken, R"({"render_max_seconds":2})");
      ++runs;
    }
    CHECK(runs > 0);
  }
  CHECK_STATUS(test::read_as<ir::Audio>("mid", as_bytes(std::string_view("MThd\0\0\0\6\0\0\0\1\0\0", 14))), Status::Decode);
}

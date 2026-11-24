// Convertible core - audio_lossless family: Audio -> Image bridge.
//
// Renders any audio as a picture - a waveform (default) or a spectrogram -
// so that every image writer can take audio ("song.flac -> song.png").
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <charconv>
#include <cstdint>
#include <string>
#include <utility>

#include "engine/registry.hpp"
#include "families/audio_lossless/audio_api.hpp"
#include "families/audio_lossless/common.hpp"
#include "util/text.hpp"

namespace cvt::audio {

namespace {

Result<std::uint32_t> colour_option(const Options& options, std::string_view key, std::string_view fallback) {
  std::string text = options.get_string(key, fallback);
  if (!text.empty() && text.front() == '#') text.erase(text.begin());
  std::uint32_t rgb = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), rgb, 16);
  if (text.size() != 6 || error != std::errc{} || end != text.data() + text.size()) {
    return fail(Status::InvalidArgument, "option '{}' must be six hexadecimal digits (RRGGBB)", key);
  }
  return rgb;
}

Result<AnyIR> render(AnyIR value, Context& ctx) {
  const ir::Audio audio = std::move(value).get<ir::Audio>();
  const std::string kind = ctx.options.get_enum("picture_kind", "waveform", {"waveform", "spectrogram"});
  const auto width = static_cast<std::uint32_t>(ctx.options.get_int("picture_width", 1200, 16, 8192));
  const auto height = static_cast<std::uint32_t>(ctx.options.get_int("picture_height", 400, 16, 8192));
  CVT_TRY(ctx.limits.check_pixels(width, height));
  if (kind == "spectrogram") {
    SpectrogramStyle style;
    style.width = width;
    style.height = height;
    const std::string fft = ctx.options.get_enum("spectrum_fft_size", "2048", {"256", "512", "1024", "2048", "4096", "8192"});
    style.fft_size = static_cast<std::uint32_t>(util::parse_int(fft).value_or(2048));
    style.range_db = ctx.options.get_float("spectrum_range_db", 120.0, 20.0, 200.0);
    style.log_frequency = ctx.options.get_bool("spectrum_log_frequency", false);
    style.palette = ctx.options.get_enum("spectrum_palette", "magma", {"magma", "viridis", "gray"});
    CVT_TRY_ASSIGN(ir::Image image, render_spectrogram(audio, style, &ctx));
    return image;
  }
  WaveformStyle style;
  style.width = width;
  style.height = height;
  CVT_TRY_ASSIGN(style.foreground, colour_option(ctx.options, "wave_color", "3b82f6"));
  CVT_TRY_ASSIGN(style.background, colour_option(ctx.options, "wave_background", "ffffff"));
  style.transparent = ctx.options.get_bool("wave_transparent", false);
  style.split_channels = ctx.options.get_bool("wave_split_channels", false);
  CVT_TRY_ASSIGN(ir::Image image, render_waveform(audio, style));
  return image;
}

}  // namespace

void register_bridges(Registry& registry) {
  Bridge bridge;
  bridge.from = Kind::Audio;
  bridge.to = Kind::Image;
  bridge.cost = 30;
  bridge.lossy = true;
  bridge.label = "render audio as a waveform or spectrogram picture";
  bridge.options = {
      OptionSpec::choice("picture_kind", "Picture", "waveform", {"waveform", "spectrogram"},
                         "Waveform shows loudness over time; spectrogram shows frequencies over time."),
      OptionSpec::integer("picture_width", "Picture width", 1200, 16, 8192, "Width of the picture in pixels."),
      OptionSpec::integer("picture_height", "Picture height", 400, 16, 8192, "Height of the picture in pixels."),
      OptionSpec::text("wave_color", "Waveform colour", "3b82f6", "Colour of the waveform as RRGGBB."),
      OptionSpec::text("wave_background", "Waveform background", "ffffff", "Background colour as RRGGBB."),
      OptionSpec::boolean("wave_transparent", "Transparent background", false,
                          "Leave the waveform background transparent (for formats with alpha).")
          .advanced(),
      OptionSpec::boolean("wave_split_channels", "One lane per channel", false,
                          "Draw every channel separately instead of a mono mix.")
          .advanced(),
      OptionSpec::choice("spectrum_palette", "Spectrogram colours", "magma", {"magma", "viridis", "gray"},
                         "Colour map of the spectrogram.")
          .advanced(),
      OptionSpec::boolean("spectrum_log_frequency", "Logarithmic frequency axis", false,
                          "Give low frequencies more room, like a musical scale.")
          .advanced(),
      OptionSpec::choice("spectrum_fft_size", "Spectrogram resolution", "2048",
                         {"256", "512", "1024", "2048", "4096", "8192"},
                         "Analysis window: larger resolves frequencies better, smaller resolves time better.")
          .advanced(),
      OptionSpec::number("spectrum_range_db", "Spectrogram range (dB)", 120.0, 20.0, 200.0,
                         "Levels this far below full scale are drawn black.")
          .advanced(),
  };
  bridge.fn = render;
  registry.add_bridge(std::move(bridge));
}

}  // namespace cvt::audio

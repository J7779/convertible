// Convertible core - audio_lossy family: registration of the synthesised-
// music area: MIDI (audio, event list as data and as table) and tracker
// modules (MOD, S3M, XM, IT).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include "engine/registry.hpp"
#include "families/audio_lossy/lossy.hpp"
#include "families/audio_lossy/synth/synth.hpp"
#include "ir/data.hpp"
#include "ir/table.hpp"
#include "util/text.hpp"

namespace cvt::lossy {

namespace {

std::vector<OptionSpec> render_options() {
  return {OptionSpec::integer("render_sample_rate", "Render sample rate", 44100, 8000, 192000,
                              "Sample rate of the synthesised audio."),
          OptionSpec::integer("render_max_seconds", "Maximum length", 600, 1, 7200,
                              "Rendering stops after this many seconds (songs that loop forever are cut).")
              .advanced()};
}

synth::RenderSettings settings_from(const Context& ctx) {
  synth::RenderSettings s;
  s.sample_rate = static_cast<std::uint32_t>(ctx.options.get_int("render_sample_rate", 44100));
  s.max_seconds = static_cast<double>(ctx.options.get_int("render_max_seconds", 600));
  return s;
}

const char* kind_name(const synth::MidiEvent& e) {
  if (e.status == 0xFF) {
    switch (e.meta) {
      case 0x01: return "text";
      case 0x02: return "copyright";
      case 0x03: return "track_name";
      case 0x04: return "instrument_name";
      case 0x05: return "lyrics";
      case 0x06: return "marker";
      case 0x07: return "cue_marker";
      case 0x20: return "channel_prefix";
      case 0x21: return "midi_port";
      case 0x2F: return "end_of_track";
      case 0x51: return "set_tempo";
      case 0x54: return "smpte_offset";
      case 0x58: return "time_signature";
      case 0x59: return "key_signature";
      case 0x7F: return "sequencer_specific";
      default: return "meta";
    }
  }
  if (e.status == 0xF0 || e.status == 0xF7) return "sysex";
  switch (e.status & 0xF0) {
    case 0x80: return "note_off";
    case 0x90: return (e.data.size() > 1 && e.data[1] == 0) ? "note_off" : "note_on";
    case 0xA0: return "polytouch";
    case 0xB0: return "control_change";
    case 0xC0: return "program_change";
    case 0xD0: return "aftertouch";
    case 0xE0: return "pitchwheel";
    default: return "unknown";
  }
}

std::string data_text(const synth::MidiEvent& e) {
  if (e.status == 0xFF && e.meta >= 0x01 && e.meta <= 0x07) {
    return util::decode_text(ByteView(reinterpret_cast<const std::byte*>(e.data.data()), e.data.size()),
                             util::TextEncoding::Windows1252)
        .text;
  }
  std::string s;
  for (std::size_t i = 0; i < e.data.size(); ++i) s += std::format("{}{}", i == 0 ? "" : " ", e.data[i]);
  return s;
}

Result<AnyIR> read_midi_audio(ReadContext& ctx) {
  CVT_TRY_ASSIGN(synth::MidiFile midi, synth::parse_midi(ctx.input, ctx));
  CVT_TRY_ASSIGN(ir::Audio audio, synth::render_midi(midi, settings_from(ctx), ctx));
  return audio;
}

Result<AnyIR> read_midi_data(ReadContext& ctx) {
  CVT_TRY_ASSIGN(synth::MidiFile midi, synth::parse_midi(ctx.input, ctx));
  Json events = Json::array();
  for (const synth::MidiEvent& e : midi.events) {
    Json item = Json::object();
    item.set("track", e.track);
    item.set("tick", e.tick);
    item.set("seconds", e.seconds);
    item.set("type", kind_name(e));
    if (e.status >= 0x80 && e.status < 0xF0) {
      item.set("channel", e.status & 0x0F);
      const int kind = e.status & 0xF0;
      if (kind == 0x80 || kind == 0x90 || kind == 0xA0) {
        item.set("note", e.data.empty() ? 0 : e.data[0]);
        item.set("velocity", e.data.size() > 1 ? e.data[1] : 0);
      } else if (kind == 0xB0) {
        item.set("control", e.data.empty() ? 0 : e.data[0]);
        item.set("value", e.data.size() > 1 ? e.data[1] : 0);
      } else if (kind == 0xC0) {
        item.set("program", e.data.empty() ? 0 : e.data[0]);
      } else if (kind == 0xE0) {
        item.set("pitch", e.data.size() > 1 ? ((e.data[1] << 7) | e.data[0]) - 8192 : 0);
      } else {
        item.set("value", e.data.empty() ? 0 : e.data[0]);
      }
    } else if (e.status == 0xFF && e.meta == 0x51 && e.data.size() == 3) {
      item.set("tempo", (e.data[0] << 16) | (e.data[1] << 8) | e.data[2]);
    } else {
      item.set("data", data_text(e));
    }
    events.push_back(std::move(item));
  }
  Json root = Json::object();
  root.set("format", midi.format);
  root.set("tracks", midi.tracks);
  root.set("ticks_per_beat", midi.division);
  root.set("duration_seconds", midi.duration);
  root.set("events", std::move(events));
  ir::Data data;
  data.value = std::move(root);
  return data;
}

Result<AnyIR> read_midi_table(ReadContext& ctx) {
  CVT_TRY_ASSIGN(synth::MidiFile midi, synth::parse_midi(ctx.input, ctx));
  ir::Sheet sheet;
  sheet.name = "events";
  sheet.has_header = true;
  sheet.rows.push_back({ir::Cell::string("track"), ir::Cell::string("tick"), ir::Cell::string("seconds"),
                        ir::Cell::string("type"), ir::Cell::string("channel"), ir::Cell::string("data")});
  for (const synth::MidiEvent& e : midi.events) {
    std::vector<ir::Cell> row;
    row.push_back(ir::Cell::numeric(e.track, std::to_string(e.track)));
    row.push_back(ir::Cell::numeric(static_cast<double>(e.tick), std::to_string(e.tick)));
    row.push_back(ir::Cell::numeric(e.seconds, std::format("{:.6f}", e.seconds)));
    row.push_back(ir::Cell::string(kind_name(e)));
    if (e.status >= 0x80 && e.status < 0xF0) row.push_back(ir::Cell::numeric(e.status & 0x0F, std::to_string(e.status & 0x0F)));
    else row.push_back(ir::Cell{});
    row.push_back(ir::Cell::string(data_text(e)));
    sheet.rows.push_back(std::move(row));
  }
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

void add_module(Registry& registry, const std::string& id, std::string name, std::string description,
                std::vector<std::string> ext, std::string mime, int popularity) {
  FormatInfo info;
  info.id = id;
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Audio;
  info.extensions = std::move(ext);
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = [id](ByteView d, std::string_view) { return synth::sniff_module(d, id); };
  registry.add_format(std::move(info));
  Codec codec;
  codec.format_id = id;
  codec.kind = Kind::Audio;
  codec.read = [id](ReadContext& ctx) -> Result<AnyIR> {
    CVT_TRY_ASSIGN(ir::Audio audio, synth::render_module(ctx.input, id, settings_from(ctx), ctx));
    return audio;
  };
  codec.read_cost = 30;
  codec.read_options = render_options();
  registry.add_codec(std::move(codec));
}

}  // namespace

void register_synth(Registry& registry) {
  FormatInfo mid;
  mid.id = "mid";
  mid.name = "MIDI music";
  mid.description = "Standard MIDI File (also karaoke .kar and RIFF .rmi): notes rendered with a built-in synthesiser.";
  mid.category = Category::Audio;
  mid.extensions = {"mid", "midi", "kar", "rmi"};
  mid.mime = "audio/midi";
  mid.popularity = 40;
  mid.sniff = [](ByteView d, std::string_view) { return synth::looks_like_midi(d) ? 95 : 0; };
  registry.add_format(std::move(mid));

  Codec audio;
  audio.format_id = "mid";
  audio.kind = Kind::Audio;
  audio.read = read_midi_audio;
  audio.read_cost = 30;
  audio.read_options = render_options();
  registry.add_codec(std::move(audio));
  Codec data;
  data.format_id = "mid";
  data.kind = Kind::Data;
  data.read = read_midi_data;
  registry.add_codec(std::move(data));
  Codec table;
  table.format_id = "mid";
  table.kind = Kind::Table;
  table.read = read_midi_table;
  registry.add_codec(std::move(table));

  add_module(registry, "mod", "ProTracker module", "Amiga tracker music (samples and patterns), rendered to audio.", {"mod"},
             "audio/x-mod", 15);
  add_module(registry, "s3m", "Scream Tracker 3 module", "Tracker music from Scream Tracker 3, rendered to audio.", {"s3m"},
             "audio/x-s3m", 8);
  add_module(registry, "xm", "FastTracker 2 module", "Tracker music with instruments and envelopes, rendered to audio.",
             {"xm"}, "audio/x-xm", 10);
  add_module(registry, "it", "Impulse Tracker module", "Tracker music from Impulse Tracker, rendered to audio.", {"it"},
             "audio/x-it", 8);
}

}  // namespace cvt::lossy

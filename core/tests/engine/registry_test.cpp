// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <memory>

#include "codec/ids.hpp"
#include "engine/fakes.hpp"
#include "engine/registry.hpp"
#include "families/families.hpp"

using namespace cvt;
using test::fake_bridge;
using test::fake_codec;
using test::fake_format;

namespace {

bool mentions(const std::vector<std::string>& problems, std::string_view needle) {
  return std::ranges::any_of(problems, [&](const std::string& p) { return p.find(needle) != std::string::npos; });
}

FormatInfo family_png() {
  FormatInfo info;
  info.id = "PNG";  // ids are normalised
  info.name = "Portable Network Graphics";
  info.extensions = {".PNG", "apng"};
  info.signatures = {Signature::at(0, "89 'PNG'")};
  return info;
}

FormatInfo database_png() {
  FormatInfo info;
  info.id = "png";
  info.name = "PNG image";
  info.description = "from the database";
  info.category = Category::Image;
  info.extensions = {"png", "pngx"};
  info.mime = "image/png";
  info.popularity = 95;
  info.signatures = {Signature::at(0, "89 'PNG' 0D 0A 1A 0A"), Signature::at(1, "'PNG'")};
  info.sniff = [](ByteView, std::string_view) { return 1; };
  return info;
}

void check_merged_png(const Registry& registry) {
  const FormatInfo* png = registry.find("png");
  REQUIRE(png != nullptr);
  CHECK(png->name == "Portable Network Graphics");       // the family wins
  CHECK(png->description == "from the database");        // the database fills gaps
  CHECK(png->category == Category::Image);
  CHECK(png->mime == "image/png");
  CHECK(png->popularity == 95);
  CHECK(png->extensions == std::vector<std::string>{"png", "apng", "pngx"});  // family's first, then the rest
  REQUIRE(png->signatures.size() == 1);                   // the family's detection is authoritative
  CHECK(png->signatures[0].bytes.size() == 4);
  CHECK_FALSE(static_cast<bool>(png->sniff));
}

struct NullVideoDecoder final : codec::VideoDecoder {
  Result<void> configure(const codec::CodecParams&) override { return {}; }
  Result<void> send_packet(const codec::Packet&) override { return {}; }
  Result<std::optional<codec::VideoFrame>> receive_frame() override { return std::nullopt; }
  Result<void> flush() override { return {}; }
};

}  // namespace

TEST_CASE("registry: a family description overrides the known-formats database in either order") {
  Registry family_first;
  family_first.add_format(family_png());
  family_first.add_known_format(database_png());
  check_merged_png(family_first);

  Registry database_first;
  database_first.add_known_format(database_png());
  database_first.add_format(family_png());
  check_merged_png(database_first);
}

TEST_CASE("registry: database-only formats keep everything the database says") {
  Registry registry;
  registry.add_known_format(database_png());
  registry.add_known_format(database_png());  // a second row for the same id merges
  const FormatInfo* png = registry.find("png");
  REQUIRE(png != nullptr);
  CHECK(png->name == "PNG image");
  CHECK(png->signatures.size() == 4);
  CHECK(static_cast<bool>(png->sniff));
  CHECK_FALSE(registry.has_reader("png"));
  CHECK_FALSE(registry.has_writer("png"));
}

TEST_CASE("registry: repeated add_format calls merge, later values win") {
  Registry registry;
  FormatInfo first = fake_format("html", Category::Document, 80, {"html", "htm"});
  first.mime = "text/html";
  registry.add_format(first);
  FormatInfo second;
  second.id = "html";
  second.name = "HTML page";
  second.extensions = {"xhtml", "html"};
  second.popularity = 500;  // clamped
  registry.add_format(second);
  const FormatInfo* html = registry.find("html");
  REQUIRE(html != nullptr);
  CHECK(html->name == "HTML page");
  CHECK(html->mime == "text/html");
  CHECK(html->category == Category::Document);
  CHECK(html->popularity == 100);
  CHECK(html->extensions == std::vector<std::string>{"xhtml", "html", "htm"});
  FormatInfo unnamed;
  registry.add_format(unnamed);  // no id: ignored
  CHECK(registry.formats().size() == 1);
}

TEST_CASE("registry: lookups") {
  Registry registry;
  registry.add_format(fake_format("jpg", Category::Image, 95, {"jpg", "jpeg"}));
  registry.add_format(fake_format("tiff", Category::Image, 60, {"tiff", "tif"}));
  FormatInfo variant = fake_format("tif-variant", Category::Image, 99, {"tv", "tif"});
  variant.signatures = {Signature::at(0, "'TV'")};  // two formats may share an extension if one can be told by content
  registry.add_format(variant);
  registry.add_codec(fake_codec("jpg", Kind::Image, true, false));
  registry.add_codec(fake_codec("jpg", Kind::Image, false, true));
  registry.add_codec(fake_codec("JPG", Kind::Blob, false, true));
  registry.add_codec(fake_codec("raw", Kind::Blob, true, false));
  registry.finalize();

  CHECK(registry.find("jpg") != nullptr);
  CHECK(registry.find("JPG") == nullptr);  // find is exact
  CHECK(registry.resolve(" .JPEG ") == registry.find("jpg"));
  CHECK(registry.resolve("Jpg") == registry.find("jpg"));
  CHECK(registry.resolve("tif") == registry.find("tif-variant"));  // see the ranking below
  CHECK(registry.resolve("tiff") == registry.find("tiff"));
  CHECK(registry.resolve("nope") == nullptr);
  CHECK(registry.resolve("") == nullptr);

  const auto by_extension = registry.formats_with_extension("tif");
  REQUIRE(by_extension.size() == 2);
  CHECK(by_extension[0]->id == "tif-variant");  // neither has it as primary: more popular first

  const auto all = registry.formats();
  REQUIRE(all.size() == 4);
  CHECK(all[0]->id == "tif-variant");
  CHECK(all[1]->id == "jpg");
  CHECK(all[3]->id == "raw");  // created by finalize() for the codec without a format
  CHECK(registry.find("raw")->name == "RAW");
  CHECK(registry.find("raw")->extensions == std::vector<std::string>{"raw"});

  CHECK(registry.codecs().size() == 4);
  CHECK(registry.codecs_for("jpg").size() == 3);
  CHECK(registry.codecs_for("tiff").empty());
  CHECK(registry.has_reader("jpg"));
  CHECK(registry.has_writer("jpg"));
  CHECK(registry.has_reader("raw"));
  CHECK_FALSE(registry.has_writer("raw"));
  CHECK(registry.validate().empty());
}

TEST_CASE("registry: validate reports registration mistakes") {
  Registry registry;
  registry.add_format(fake_format("ok"));
  registry.add_format(fake_format("bad id!"));
  FormatInfo dotted = fake_format("dotted");
  dotted.extensions = {"dotted"};
  dotted.signatures.push_back(Signature{0, {}, {}});
  dotted.signatures.push_back(Signature{0, to_bytes("ab"), to_bytes("a")});
  registry.add_format(dotted);
  registry.add_format(fake_format("twin-a", Category::Other, 1, {"shared"}));
  registry.add_format(fake_format("twin-b", Category::Other, 1, {"shared"}));

  Codec no_functions;
  no_functions.format_id = "ok";
  registry.add_codec(no_functions);
  Codec bad_cost = fake_codec("ok", Kind::Blob, true, true, 0, 10);
  bad_cost.write_options = {OptionSpec::integer("limits", "Limits", 1, 0, 2),
                            OptionSpec::integer("level", "Level", 50, 0, 9),
                            OptionSpec::choice("mode", "Mode", "x", {})};
  registry.add_codec(bad_cost);
  registry.add_codec(fake_codec("ghost", Kind::Blob, true, false));  // no finalize(): unknown format

  Bridge loop = fake_bridge(Kind::Image, Kind::Image, 0);
  registry.add_bridge(loop);
  Bridge empty;
  empty.from = Kind::Image;
  empty.to = Kind::Blob;
  registry.add_bridge(empty);
  registry.media().register_video_decoder("not-a-codec", [] { return std::make_unique<NullVideoDecoder>(); });

  const std::vector<std::string> problems = registry.validate();
  CHECK(mentions(problems, "'bad id!'"));
  CHECK(mentions(problems, "empty signature"));
  CHECK(mentions(problems, "mask length"));
  CHECK(mentions(problems, "both claim extension 'shared'"));
  CHECK(mentions(problems, "neither read nor write"));
  CHECK(mentions(problems, "non-positive cost"));
  CHECK(mentions(problems, "'limits' is reserved"));
  CHECK(mentions(problems, "default of option 'level'"));
  CHECK(mentions(problems, "enum option 'mode' has no choices"));
  CHECK(mentions(problems, "unknown format 'ghost'"));
  CHECK(mentions(problems, "-> itself"));
  CHECK(mentions(problems, "has no function"));
  CHECK(mentions(problems, "not a canonical codec id"));
}

TEST_CASE("registry: signature patterns") {
  static_assert(detail::signature_pattern_length("89 'PNG' 0D 0A 1A 0A") == 8);
  static_assert(detail::signature_pattern_length("'RIFF' ?? ?? ?? ?? 'WEBP'") == 12);
  static_assert(detail::signature_pattern_length("FF&E0 4? ?F") == 3);
  static_assert(detail::signature_pattern_length("") < 0);
  static_assert(detail::signature_pattern_length("8") < 0);
  static_assert(detail::signature_pattern_length("GG") < 0);
  static_assert(detail::signature_pattern_length("'open") < 0);
  static_assert(detail::signature_pattern_length("''") < 0);
  static_assert(detail::signature_pattern_length("8989") < 0);
  static_assert(detail::signature_pattern_length("??&FF") < 0);
  static_assert(detail::signature_pattern_length("FF&G0") < 0);

  const Signature png = Signature::at(0, "89 'PNG' 0D 0A 1A 0A");
  CHECK(png.bytes.size() == 8);
  CHECK(png.mask.empty());
  CHECK(png.significant_bits() == 64);
  CHECK(png.matches(as_bytes("\x89PNG\r\n\x1a\n....")));
  CHECK_FALSE(png.matches(as_bytes("\x89PNG\r\n\x1a")));  // too short
  CHECK_FALSE(png.matches(as_bytes("\x89PNG\r\n\x1a\r")));
  CHECK_FALSE(png.matches({}));

  const Signature webp = Signature::at(0, "'RIFF' ?? ?? ?? ?? 'WEBP'");
  CHECK(webp.significant_bits() == 64);
  CHECK(webp.matches(as_bytes("RIFF\x01\x02\x03\x04WEBPVP8 ")));
  CHECK_FALSE(webp.matches(as_bytes("RIFF\x01\x02\x03\x04WAVEfmt ")));

  const Signature masked = Signature::at(2, "FF&E0 4? ?F");
  CHECK(masked.significant_bits() == 11);
  CHECK(masked.matches(as_bytes("..\xE7\x4A\x0F")));
  CHECK(masked.matches(as_bytes("..\xFF\x40\xFF")));
  CHECK_FALSE(masked.matches(as_bytes("..\xDF\x40\xFF")));
  CHECK_FALSE(masked.matches(as_bytes("..\xFF\x50\xFF")));
  CHECK_FALSE(masked.matches(as_bytes("..\xFF\x40\xFE")));
  CHECK_FALSE(masked.matches(as_bytes("\xFF\x40\xFF")));  // offset matters

  CHECK(Signature::parse(4, "'ftyp'").has_value());
  CHECK(Signature::parse(4, "'ftyp'")->offset == 4);
  CHECK_FALSE(Signature::parse(0, "zz").has_value());
  CHECK_FALSE(Signature::at(0, SignaturePattern{}).matches(as_bytes("anything")));
}

TEST_CASE("registry: category names round-trip") {
  for (const std::string_view name : {"image", "audio", "video", "document", "ebook", "spreadsheet", "presentation",
                                      "data", "archive", "font", "subtitle", "model3d", "geo", "pim", "code",
                                      "encoding", "vector", "other"}) {
    const auto category = parse_category(name);
    REQUIRE_MESSAGE(category.has_value(), name);
    CHECK(category_name(*category) == name);
  }
  CHECK(parse_category("IMAGE") == Category::Image);
  CHECK_FALSE(parse_category("nonsense").has_value());
  CHECK(normalize_format_id("  .TAR.GZ ") == "tar.gz");
  CHECK(normalize_format_id(".") == ".");
}

TEST_CASE("registry: media codec factories") {
  Registry registry;
  codec::Registry& media = registry.media();
  CHECK(media.list().empty());
  CHECK_FALSE(media.video_decoder(codec::id::h264).has_value());
  CHECK(media.make_video_decoder(codec::id::h264) == nullptr);
  CHECK(media.make_audio_encoder(codec::id::aac) == nullptr);

  CHECK(media.register_video_decoder(codec::id::h264, [] { return std::make_unique<NullVideoDecoder>(); }));
  CHECK_FALSE(media.register_video_decoder(codec::id::h264, [] { return std::make_unique<NullVideoDecoder>(); }));
  CHECK_FALSE(media.register_video_decoder(codec::id::vp9, nullptr));
  CHECK(media.has_video_decoder("h264"));
  CHECK_FALSE(media.has_video_encoder("h264"));
  CHECK_FALSE(media.has_audio_decoder("h264"));
  REQUIRE(media.video_decoder("h264").has_value());
  const auto first = media.make_video_decoder("h264");
  const auto second = media.make_video_decoder("h264");
  REQUIRE(first != nullptr);
  CHECK(first != second);  // a fresh object per call
  CHECK(first->receive_frame().has_value());
  REQUIRE(media.list().size() == 1);
  CHECK(media.list()[0].id == "h264");
  CHECK(media.list()[0].role == codec::CodecRole::VideoDecoder);
  CHECK(media.problems().size() == 2);  // the duplicate and the empty factory

  static_assert(codec::id::is_known("h264") && codec::id::is_known("pcm_s16le") && !codec::id::is_known("H264"));
  for (const std::string_view id : codec::id::video) CHECK(codec::id::is_known(id));
  for (const std::string_view id : codec::id::audio) CHECK(codec::id::is_known(id));
}

TEST_CASE("registry: the shared instance is complete and sound") {
  const Registry& registry = Registry::instance();
  CHECK(&registry == &Registry::instance());
  const std::vector<std::string> problems = registry.validate();
  for (const std::string& problem : problems) FAIL_CHECK(problem);
  CHECK(problems.empty());

  // The encoding family and the known-formats database are both present.
  CHECK(registry.has_writer("base64"));
  CHECK(registry.has_reader("txt"));
  REQUIRE(registry.find("png") != nullptr);
  CHECK(registry.find("png")->category == Category::Image);
  CHECK(registry.formats().size() >= 100);

  // A second, independently built registry has the same content: registration is deterministic.
  Registry again;
  register_all(again);
  again.finalize();
  CHECK(again.formats().size() == registry.formats().size());
  CHECK(again.codecs().size() == registry.codecs().size());
  CHECK(again.bridges().size() == registry.bridges().size());
}

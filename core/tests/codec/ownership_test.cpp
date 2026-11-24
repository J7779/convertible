// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
// The ownership rules of codec::Registry and Registry::validate(): one owner
// per (codec id, role), fallbacks that never displace an owner whatever the
// registration order, and one reader and one writer per (format, kind).
#include "support/test_support.hpp"

#include "codec/registry.hpp"
#include "engine/fakes.hpp"

using namespace cvt;

namespace {

std::unique_ptr<codec::AudioDecoder> none() { return nullptr; }

}  // namespace

TEST_CASE("codec registry: an owner replaces a fallback in either order; two owners are a problem") {
  int which = 0;
  const auto owner = [&which]() -> std::unique_ptr<codec::AudioDecoder> {
    which = 1;
    return nullptr;
  };
  const auto fallback = [&which]() -> std::unique_ptr<codec::AudioDecoder> {
    which = 2;
    return nullptr;
  };
  {
    codec::Registry media;
    CHECK(media.register_audio_decoder_fallback("pcm_s16le", fallback));
    CHECK(media.register_audio_decoder("pcm_s16le", owner));
    (void)media.make_audio_decoder("pcm_s16le");
    CHECK(which == 1);
    CHECK_FALSE(media.is_fallback("pcm_s16le", codec::CodecRole::AudioDecoder));
    CHECK(media.problems().empty());
  }
  {
    codec::Registry media;
    CHECK(media.register_audio_decoder("pcm_s16le", owner));
    CHECK_FALSE(media.register_audio_decoder_fallback("pcm_s16le", fallback));
    which = 0;
    (void)media.make_audio_decoder("pcm_s16le");
    CHECK(which == 1);
    CHECK(media.problems().empty());
  }
  {
    codec::Registry media;
    CHECK(media.register_audio_decoder("pcm_s16le", owner));
    CHECK_FALSE(media.register_audio_decoder("pcm_s16le", none));
    REQUIRE(media.problems().size() == 1);
    CHECK(media.problems()[0].find("registered twice") != std::string::npos);
  }
}

TEST_CASE("registry: two readers or writers of one format and kind are reported") {
  Registry registry;
  registry.add_format(test::fake_format("clip"));
  registry.add_codec(test::fake_codec("clip", Kind::Video, true, true));
  CHECK(registry.validate().empty());
  registry.add_codec(test::fake_codec("clip", Kind::Image, true, false));  // another kind: fine
  CHECK(registry.validate().empty());
  registry.add_codec(test::fake_codec("clip", Kind::Video, true, false));
  const auto problems = registry.validate();
  REQUIRE(problems.size() == 1);
  CHECK(problems[0].find("two Video readers") != std::string::npos);
}

TEST_CASE("registry: a family format beats a database entry on a shared untested extension") {
  Registry registry;
  FormatInfo family;
  family.id = "fwf";
  family.name = "Fixed width";
  family.extensions = {"fwf", "prn"};
  registry.add_format(family);
  FormatInfo known;
  known.id = "pcl";
  known.name = "PCL print job";
  known.extensions = {"pcl", "prn"};
  known.popularity = 90;
  registry.add_known_format(known);
  registry.finalize();
  CHECK(registry.validate().empty());
  const auto candidates = registry.formats_with_extension("prn");
  REQUIRE(candidates.size() == 2);
  CHECK(candidates.front()->id == "fwf");
}

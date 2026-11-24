// HEIF container structure: hand-assembled files for derived items (overlay,
// identity with rotation, JPEG-coded items) around coded data written by
// independent encoders, and hostile input (truncations, corruptions,
// structural bombs, limits, cancellation).
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/detect.hpp"
#include "engine/registry.hpp"
#include "families/image/image_ops.hpp"
#include "families/image_modern/heif_internal.hpp"
#include "ir/image.hpp"
#include "support/fixtures.hpp"
#include "support/metrics.hpp"
#include "support/test_support.hpp"
#include "util/bytes.hpp"

using namespace cvt;

namespace {

std::string data(std::string_view name) { return std::string("image_modern/heif/") + std::string(name); }

Bytes box(std::string_view type, ByteView body) {
  util::ByteWriter w;
  w.u32be(static_cast<std::uint32_t>(body.size() + 8)).text(type).bytes(body);
  return w.take();
}

Bytes full(std::string_view type, std::uint8_t version, std::uint32_t flags, ByteView body) {
  util::ByteWriter w;
  w.u8(version).u24be(flags).bytes(body);
  return box(type, w.view());
}

Bytes cat(std::initializer_list<ByteView> parts) {
  Bytes out;
  for (const ByteView part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

struct HandItem {
  std::uint16_t id;
  std::string type;
  Bytes payload;                         // stored in 'idat'
  std::vector<std::uint8_t> properties;  // 1-based ipco indices (bit 7 = essential)
};

struct HandRef {
  std::string type;
  std::uint16_t from;
  std::vector<std::uint16_t> to;
};

/// A HEIF file whose item payloads live in 'idat' (construction method 1).
Bytes heif_file(const std::vector<HandItem>& items, const std::vector<Bytes>& properties,
                const std::vector<HandRef>& refs, std::uint16_t primary, std::uint64_t forced_extent_offset = 0) {
  util::ByteWriter ftyp;
  ftyp.text("mif1").u32be(0).text("mif1").text("miaf");
  util::ByteWriter hdlr;
  hdlr.u32be(0).text("pict").u32be(0).u32be(0).u32be(0).u8(0);
  util::ByteWriter pitm;
  pitm.u16be(primary);
  util::ByteWriter iinf;
  iinf.u16be(static_cast<std::uint16_t>(items.size()));
  for (const HandItem& item : items) {
    util::ByteWriter infe;
    infe.u16be(item.id).u16be(0).text(item.type).u8(0);
    iinf.bytes(full("infe", 2, 0, infe.view()));
  }
  util::ByteWriter iloc;
  iloc.u8(0x44).u8(0x00).u16be(static_cast<std::uint16_t>(items.size()));
  util::ByteWriter idat;
  for (const HandItem& item : items) {
    iloc.u16be(item.id).u16be(1).u16be(0).u16be(1);  // construction method 1, one extent
    iloc.u32be(static_cast<std::uint32_t>(forced_extent_offset != 0 ? forced_extent_offset : idat.size()))
        .u32be(static_cast<std::uint32_t>(item.payload.size()));
    idat.bytes(item.payload);
  }
  util::ByteWriter iref;
  for (const HandRef& ref : refs) {
    util::ByteWriter r;
    r.u16be(ref.from).u16be(static_cast<std::uint16_t>(ref.to.size()));
    for (const std::uint16_t t : ref.to) r.u16be(t);
    iref.bytes(box(ref.type, r.view()));
  }
  util::ByteWriter ipco;
  for (const Bytes& p : properties) ipco.bytes(p);
  util::ByteWriter ipma;
  ipma.u32be(static_cast<std::uint32_t>(items.size()));
  for (const HandItem& item : items) {
    ipma.u16be(item.id).u8(static_cast<std::uint8_t>(item.properties.size()));
    for (const std::uint8_t p : item.properties) ipma.u8(p);
  }
  const Bytes iprp = box("iprp", cat({box("ipco", ipco.view()), full("ipma", 0, 0, ipma.view())}));
  const Bytes meta = full("meta", 0, 0,
                          cat({full("hdlr", 0, 0, hdlr.view()), full("pitm", 0, 0, pitm.view()),
                               full("iinf", 0, 0, iinf.view()), full("iloc", 1, 0, iloc.view()),
                               full("iref", 0, 0, iref.view()), iprp, box("idat", idat.view())}));
  return cat({box("ftyp", ftyp.view()), meta});
}

Bytes ispe(std::uint32_t w, std::uint32_t h) {
  util::ByteWriter b;
  b.u32be(w).u32be(h);
  return full("ispe", 0, 0, b.view());
}

Bytes grid_payload(std::uint32_t rows, std::uint32_t columns, std::uint32_t w, std::uint32_t h) {
  util::ByteWriter b;
  b.u8(0).u8(1).u8(static_cast<std::uint8_t>(rows - 1)).u8(static_cast<std::uint8_t>(columns - 1)).u32be(w).u32be(h);
  return b.take();
}

/// The coded picture and its 'av1C' payload of the primary item of a libheif-written AVIF.
struct CodedAv1 {
  Bytes data;
  Bytes av1c;
};
CodedAv1 av1_from_fixture(std::string_view name) {
  const Bytes file = test::fixture(data(name));
  CodedAv1 out;
  const auto parsed = image_modern::heif::parse_file(file, Limits{});
  if (!parsed) return out;
  const auto* item = parsed->item(parsed->primary);
  if (item == nullptr) return out;
  const auto bytes = image_modern::heif::item_data(*parsed, *item);
  if (!bytes) return out;
  out.data = to_bytes(bytes->view);
  if (const auto* config = parsed->property(*item, image_modern::heif::fourcc("av1C")); config != nullptr) {
    out.av1c = to_bytes(config->body);
  }
  return out;
}

}  // namespace

TEST_CASE("image_modern/heif: overlay ('iovl') items composite their inputs on the fill colour") {
  const CodedAv1 coded = av1_from_fixture("avif_420.avif");
  REQUIRE_FALSE(coded.data.empty());
  const ir::Image picture = test::read_as<ir::Image>("avif", test::fixture(data("avif_420.avif"))).value();
  util::ByteWriter iovl;
  iovl.u8(0).u8(0).u16be(65535).u16be(0).u16be(0).u16be(65535);  // opaque red canvas
  iovl.u16be(100).u16be(60);
  iovl.i16be(10).i16be(5).i16be(50).i16be(20);
  const Bytes file = heif_file({{1, "iovl", iovl.take(), {1}}, {2, "av01", coded.data, {0x82, 3}}, {3, "av01", coded.data, {0x82, 3}}},
                               {ispe(100, 60), box("av1C", coded.av1c), ispe(64, 48)}, {{"dimg", 1, {2, 3}}}, 1);
  const auto image = test::read_as<ir::Image>("heif", file);
  REQUIRE_OK(image);
  REQUIRE(image->width() == 100);
  REQUIRE(image->height() == 60);
  const ir::Frame& f = image->frames[0];
  const ir::Frame& p = picture.frames[0];
  const auto same = [&](std::uint32_t x, std::uint32_t y, std::uint32_t px, std::uint32_t py) {
    for (std::size_t c = 0; c < 4; ++c) {
      if (f.rgba[f.offset(x, y) + c] != p.rgba[p.offset(px, py) + c]) return false;
    }
    return true;
  };
  CHECK(f.rgba[f.offset(2, 2)] == 255);
  CHECK(f.rgba[f.offset(2, 2) + 1] == 0);
  CHECK(same(12, 7, 2, 2));     // first input only
  CHECK(same(60, 30, 10, 10));  // the second input lies on top
  CHECK(same(99, 59, 49, 39));
}

TEST_CASE("image_modern/heif: identity ('iden') items apply their own rotation") {
  const CodedAv1 coded = av1_from_fixture("avif_420.avif");
  const ir::Image picture = test::read_as<ir::Image>("avif", test::fixture(data("avif_420.avif"))).value();
  const Bytes irot = box("irot", test::bytes("\x01"));  // 90 degrees anticlockwise
  const Bytes file = heif_file({{1, "iden", {}, {0x83, 1}}, {2, "av01", coded.data, {0x82, 1}}},
                               {ispe(64, 48), box("av1C", coded.av1c), irot}, {{"dimg", 1, {2}}}, 1);
  const auto image = test::read_as<ir::Image>("avif", file);
  REQUIRE_OK(image);
  CHECK(image->width() == 48);
  CHECK(image->height() == 64);
  CHECK(test::pixels_equal(image->frames[0], image::rotate(picture.frames[0], 270)));
}

TEST_CASE("image_modern/heif: JPEG-coded items are decoded by the JPEG codec") {
  const Bytes jpeg = test::fixture(data("card_64x48.jpg"));  // written by ImageMagick
  const Bytes file = heif_file({{1, "jpeg", jpeg, {1}}}, {ispe(64, 48)}, {}, 1);
  const auto image = test::read_as<ir::Image>("heif", file);
  REQUIRE_OK(image);
  const auto direct = test::read_as<ir::Image>("jpg", jpeg);
  REQUIRE_OK(direct);
  CHECK(test::pixels_equal(image->frames[0], direct->frames[0]));
  const auto found = detect_format(Registry::instance(), file, "");
  REQUIRE(found.has_value());
  CHECK(found->format->id == "heif");  // major brand mif1 without an image brand
}

TEST_CASE("image_modern/heif: truncated and corrupted files fail cleanly") {
  for (const auto& [format, file] :
       {std::pair{"avif", "avif_420.avif"}, std::pair{"avif", "avif_alpha.avif"}, std::pair{"avif", "avif_grid.avif"},
        std::pair{"avif", "avif_sequence.avif"}, std::pair{"heic", "heic_grid.heic"},
        std::pair{"heic", "heic_sips_exif6.heic"}, std::pair{"heic", "heic_alpha.heic"},
        std::pair{"heic", "heic_10bit.heic"}}) {
    INFO(file);
    const Bytes bytes = test::fixture(data(file));
    CHECK(test::fuzz_reader<ir::Image>(format, bytes, 256) > 0);
    CHECK_STATUS(test::read_as<ir::Image>(format, ByteView(bytes).first(bytes.size() / 2)), Status::Decode);
  }
  CHECK_STATUS(test::read_as<ir::Image>("heic", test::bytes("not a heif file at all")), Status::Decode);
  CHECK_STATUS(test::read_as<ir::Image>("avif", Bytes{}), Status::Decode);
}

TEST_CASE("image_modern/heif: limits stop oversized pictures before decoding") {
  Limits small;
  small.max_pixels = 100'000;
  CHECK_STATUS(test::read_as<ir::Image>("heic", test::fixture(data("heic_sips_grid.heic")), {}, nullptr, small),
               Status::Limit);
  Limits tiny;
  tiny.max_pixels = 1000;
  CHECK_STATUS(test::read_as<ir::Image>("avif", test::fixture(data("avif_420.avif")), {}, nullptr, tiny),
               Status::Limit);
  CHECK_STATUS(test::read_as<ir::Image>("avif", test::fixture(data("avif_sequence.avif")), {}, nullptr, tiny),
               Status::Limit);
}

TEST_CASE("image_modern/heif: hand-made structural bombs") {
  SUBCASE("a grid claiming 60000x60000 pixels") {
    const Bytes file = heif_file({{1, "grid", grid_payload(1, 1, 60000, 60000), {1}}, {2, "hvc1", Bytes(16), {2}}},
                                 {ispe(60000, 60000), ispe(64, 64)}, {{"dimg", 1, {2}}}, 1);
    CHECK_STATUS(test::read_as<ir::Image>("heic", file), Status::Limit);
  }
  SUBCASE("a small grid that repeats one tile thousands of times") {
    const CodedAv1 coded = av1_from_fixture("avif_420.avif");
    const std::vector<std::uint16_t> tiles(255 * 255, 2);
    const Bytes file = heif_file({{1, "grid", grid_payload(255, 255, 64, 48), {1}}, {2, "av01", coded.data, {0x82}}},
                                 {ispe(64, 48), box("av1C", coded.av1c)}, {{"dimg", 1, tiles}}, 1);
    Limits limits;
    limits.max_pixels = 100'000;
    CHECK_STATUS(test::read_as<ir::Image>("avif", file, {}, nullptr, limits), Status::Limit);
  }
  SUBCASE("a grid that contains itself") {
    const Bytes file = heif_file({{1, "grid", grid_payload(1, 1, 64, 64), {1}}}, {ispe(64, 64)}, {{"dimg", 1, {1}}}, 1);
    CHECK_STATUS(test::read_as<ir::Image>("heic", file), Status::Decode);
  }
  SUBCASE("two grids that contain each other") {
    const Bytes file =
        heif_file({{1, "grid", grid_payload(1, 1, 64, 64), {1}}, {2, "grid", grid_payload(1, 1, 64, 64), {1}}},
                  {ispe(64, 64)}, {{"dimg", 1, {2}}, {"dimg", 2, {1}}}, 1);
    CHECK_STATUS(test::read_as<ir::Image>("heic", file), Status::Decode);
  }
  SUBCASE("identity derivations nested too deeply") {
    std::vector<HandItem> items;
    std::vector<HandRef> refs;
    for (std::uint16_t id = 1; id <= 40; ++id) {
      items.push_back({id, "iden", {}, {}});
      refs.push_back({"dimg", id, {static_cast<std::uint16_t>(id + 1)}});
    }
    items.push_back({41, "hvc1", Bytes(8), {}});
    CHECK_STATUS(test::read_as<ir::Image>("heic", heif_file(items, {}, refs, 1)), Status::Limit);
  }
  SUBCASE("a grid with fewer tiles than cells") {
    const Bytes file = heif_file({{1, "grid", grid_payload(4, 4, 256, 256), {1}}, {2, "hvc1", Bytes(16), {2}}},
                                 {ispe(256, 256), ispe(64, 64)}, {{"dimg", 1, {2}}}, 1);
    CHECK_STATUS(test::read_as<ir::Image>("heic", file), Status::Decode);
  }
  SUBCASE("an item extent outside the data") {
    const Bytes file = heif_file({{1, "grid", grid_payload(1, 1, 64, 64), {1}}}, {ispe(64, 64)}, {}, 1, 0x7FFFFFF0u);
    CHECK_STATUS(test::read_as<ir::Image>("heic", file), Status::Decode);
  }
  SUBCASE("an unknown essential property") {
    const Bytes file = heif_file({{1, "hvc1", Bytes(16), {0x81}}}, {box("zzzz", Bytes(4))}, {}, 1);
    const auto result = test::read_as<ir::Image>("heic", file);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("zzzz") != std::string::npos);
  }
  SUBCASE("an unsupported item type and a missing primary item") {
    CHECK_STATUS(test::read_as<ir::Image>("heic", heif_file({{1, "unci", Bytes(16), {}}}, {}, {}, 1)), Status::Decode);
    CHECK_STATUS(test::read_as<ir::Image>("heic", heif_file({}, {}, {}, 7)), Status::Decode);
  }
  SUBCASE("garbage coded data") {
    const Bytes file = heif_file({{1, "av01", test::random_bytes(64, 3), {1}}}, {ispe(64, 64)}, {}, 1);
    CHECK_STATUS(test::read_as<ir::Image>("avif", file), Status::Decode);
  }
}

TEST_CASE("image_modern/heif: decoding can be cancelled") {
  const Codec* codec = test::find_reader("heic", Kind::Image);
  REQUIRE(codec != nullptr);
  const Bytes bytes = test::fixture(data("heic_sips_grid.heic"));
  ReadContext ctx;
  ctx.registry = &Registry::instance();
  ctx.input = bytes;
  ctx.format_id = "heic";
  ctx.progress = Progress([](double, std::string_view) { return true; });
  CHECK_STATUS(codec->read(ctx), Status::Cancelled);
}

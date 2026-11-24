// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <type_traits>

#include "engine/context.hpp"
#include "ir/any.hpp"
#include "support/test_support.hpp"

using namespace cvt;

static_assert(std::is_move_constructible_v<AnyIR> && std::is_move_assignable_v<AnyIR>);
static_assert(!std::is_copy_constructible_v<AnyIR>, "AnyIR is move-only because ir::Video is");
static_assert(!std::is_copy_constructible_v<ir::Video> && std::is_nothrow_move_constructible_v<ir::Video>);
static_assert(std::is_copy_constructible_v<ir::Image> && std::is_copy_constructible_v<ir::Document>);
static_assert(IrType<ir::Blob> && IrType<ir::Video> && !IrType<int> && !IrType<std::string>);
static_assert(kind_of<ir::Mesh> == Kind::Mesh && kind_of<ir::Video> == Kind::Video);
static_assert(all_kinds.size() == kind_count);
static_assert(parse_kind("Subtitles") == Kind::Subtitles && !parse_kind("subtitles").has_value());

TEST_CASE("ir: AnyIR holds one value and reports its kind") {
  AnyIR empty;
  CHECK(empty.kind() == Kind::Blob);

  ir::PlainText text;
  text.text = "hello";
  AnyIR any = text;  // copies a copyable IR
  CHECK(any.kind() == Kind::PlainText);
  CHECK(any.is<ir::PlainText>());
  CHECK_FALSE(any.is<ir::Blob>());
  CHECK(any.get<ir::PlainText>().text == "hello");
  CHECK(any.get_if<ir::Image>() == nullptr);
  REQUIRE(any.get_if<ir::PlainText>() != nullptr);
  any.get<ir::PlainText>().text += "!";
  const AnyIR& view = any;
  CHECK(view.get<ir::PlainText>().text == "hello!");

  ir::PlainText moved_out = std::move(any).get<ir::PlainText>();
  CHECK(moved_out.text == "hello!");

  AnyIR other = ir::Table{};
  other = ir::Audio{};
  CHECK(other.kind() == Kind::Audio);
  CHECK(other.visit([](const auto& value) { return sizeof(value) > 0; }));

  for (const Kind kind : all_kinds) CHECK(parse_kind(kind_name(kind)) == kind);
}

TEST_CASE("ir: a streaming Video travels through AnyIR by move") {
  ir::Video video;
  video.info.width = 4;
  video.info.height = 2;
  video.info.fps = {30000, 1001};
  int produced = 0;
  video.next_frame = [&produced]() -> Result<std::optional<ir::VideoFrame>> {
    if (produced == 3) return std::nullopt;
    ir::VideoFrame frame = ir::VideoFrame::allocate(ir::PixelFormat::I420, 4, 2);
    frame.pts_ms = produced++ * 33;
    return frame;
  };
  AnyIR any = std::move(video);
  CHECK(any.kind() == Kind::Video);
  AnyIR moved = std::move(any);
  const ir::Video& stream = moved.get<ir::Video>();
  CHECK_APPROX(stream.info.fps.value(), 29.97002997, 1e-6);
  int frames = 0;
  while (true) {
    auto frame = stream.next_frame();
    REQUIRE_OK(frame);
    if (!*frame) break;
    CHECK((*frame)->valid());
    ++frames;
  }
  CHECK(frames == 3);
  CHECK_FALSE(static_cast<bool>(stream.next_audio));
}

TEST_CASE("ir: video frame layout helpers") {
  using ir::PixelFormat;
  static_assert(ir::plane_count(PixelFormat::RGBA8) == 1 && ir::plane_count(PixelFormat::NV12) == 2 &&
                ir::plane_count(PixelFormat::I444P10) == 3);
  static_assert(ir::bit_depth(PixelFormat::I420) == 8 && ir::bit_depth(PixelFormat::P010) == 10);
  static_assert(ir::plane_width(PixelFormat::I420, 1, 5) == 3 && ir::plane_height(PixelFormat::I420, 2, 5) == 3);
  static_assert(ir::plane_width(PixelFormat::I422, 1, 5) == 3 && ir::plane_height(PixelFormat::I422, 1, 5) == 5);
  static_assert(ir::plane_width(PixelFormat::I444, 2, 5) == 5);
  static_assert(ir::plane_row_bytes(PixelFormat::RGBA8, 0, 5) == 20);
  static_assert(ir::plane_row_bytes(PixelFormat::NV12, 1, 5) == 6);
  static_assert(ir::plane_row_bytes(PixelFormat::P010, 1, 5) == 12);
  static_assert(ir::plane_row_bytes(PixelFormat::I420P10, 0, 5) == 10);

  ir::VideoFrame frame = ir::VideoFrame::allocate(PixelFormat::I420, 5, 3);
  CHECK(frame.valid());
  REQUIRE(frame.planes.size() == 3);
  CHECK(frame.planes[0].data.size() == 15);
  CHECK(frame.planes[1].data.size() == 6);
  frame.planes[2].data.pop_back();
  CHECK_FALSE(frame.valid());
  frame = ir::VideoFrame::allocate(PixelFormat::RGBA8, 2, 2);
  frame.planes[0].stride = 7;  // smaller than a row
  CHECK_FALSE(frame.valid());
  frame.planes[0].stride = std::numeric_limits<std::size_t>::max() / 2;  // must not wrap the size check
  CHECK_FALSE(frame.valid());
  CHECK_FALSE(ir::VideoFrame{}.valid());
  frame = ir::VideoFrame::allocate(PixelFormat::RGBA8, 2, 2);
  frame.planes.emplace_back();
  CHECK_FALSE(frame.valid());
}

TEST_CASE("ir: image, audio, table and mesh helpers") {
  const ir::Frame red = ir::Frame::filled(3, 2, {255, 0, 0, 255});
  CHECK(red.valid());
  CHECK(red.rgba.size() == 24);
  CHECK(red.offset(2, 1) == 20);
  CHECK_FALSE(red.has_transparency());
  CHECK(ir::Frame::filled(1, 1).has_transparency());
  CHECK_FALSE(ir::Frame{}.valid());
  ir::Image image;
  CHECK_FALSE(image.valid());
  image.frames = {red, red};
  CHECK(image.valid());
  CHECK(image.animated());
  CHECK(image.width() == 3);
  image.frames[1].rgba.pop_back();
  CHECK_FALSE(image.valid());

  ir::Audio audio;
  audio.sample_rate = 48000;
  audio.channels = 2;
  audio.samples.assign(96000, 0.0f);
  CHECK(audio.valid());
  CHECK(audio.frame_count() == 48000);
  CHECK(audio.duration_seconds() == 1.0);
  audio.samples.pop_back();
  CHECK_FALSE(audio.valid());
  CHECK(ir::Audio{}.frame_count() == 0);

  ir::Sheet sheet;
  sheet.rows = {{ir::Cell::string("a"), ir::Cell::numeric(1.5, "1.50")}, {ir::Cell::boolean(true)}, {}};
  CHECK(sheet.column_count() == 2);
  CHECK(sheet.rows[0][1].number == 1.5);
  CHECK(sheet.rows[1][0].text == "TRUE");
  CHECK(ir::Cell{}.empty());
  CHECK(ir::Cell::datetime(0.0, "1970-01-01T00:00:00Z").type == ir::CellType::DateTime);

  ir::Mesh quad;
  quad.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  quad.indices = {0, 1, 2, 3};
  quad.face_sizes = {4};
  CHECK(quad.valid());
  CHECK_FALSE(quad.is_triangulated());
  CHECK(quad.face_count() == 1);
  CHECK(quad.triangle_indices() == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
  quad.indices[3] = 9;
  CHECK_FALSE(quad.valid());
  ir::Mesh triangles;
  triangles.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  triangles.indices = {0, 1, 2};
  CHECK(triangles.valid());
  CHECK(triangles.is_triangulated());
  CHECK(triangles.triangle_indices() == triangles.indices);
  triangles.normals.resize(2);
  CHECK_FALSE(triangles.valid());

  ir::Font font;
  font.tables = {{"head", {}}, {"cvt ", {}}};
  CHECK(font.find("cvt ") == &font.tables[1]);
  CHECK(font.find("glyf") == nullptr);
}

TEST_CASE("ir: archive paths are sanitised") {
  CHECK(ir::sanitize_archive_path("dir/file.txt") == "dir/file.txt");
  CHECK(ir::sanitize_archive_path("/etc/passwd") == "etc/passwd");
  CHECK(ir::sanitize_archive_path("a\\b\\c") == "a/b/c");
  CHECK(ir::sanitize_archive_path("C:\\Windows\\x") == "Windows/x");
  CHECK(ir::sanitize_archive_path("./a//b/./c/") == "a/b/c");
  CHECK_FALSE(ir::sanitize_archive_path("../../etc/passwd").has_value());
  CHECK_FALSE(ir::sanitize_archive_path("a/../../b").has_value());
  CHECK_FALSE(ir::sanitize_archive_path("").has_value());
  CHECK_FALSE(ir::sanitize_archive_path("/").has_value());
  CHECK(ir::sanitize_archive_path(std::string_view("nul\0byte\x01.txt", 13)) == "nulbyte.txt");
}

TEST_CASE("ir: documents are built tersely and flatten to plain text") {
  using namespace ir::build;
  ir::Document document;
  document.metadata.title = "Example";
  document.blocks.push_back(heading(1, "Title"));
  document.blocks.push_back(paragraph({text("Plain, "), strong("bold"), text(" and "), emphasis({text("nested "), code("code")}),
                                       text(", a "), hyperlink("https://example.org", "link"), line_break(), text("next line")}));
  document.blocks.push_back(list(false, {{paragraph("first")}, {paragraph("second"), paragraph("more")}}));
  document.blocks.push_back(list(true, {{paragraph("one")}, {paragraph("two")}}, 9));
  document.blocks.push_back(block_quote({paragraph("quoted"), paragraph("twice")}));
  document.blocks.push_back(code_block("int x;\nint y;", "cpp"));
  document.blocks.push_back(table({row({"h1", "h2"}, true), row({"a", "b"})}));
  document.blocks.push_back(image_block(ir::EncodedImage{{}, "image/png"}, "alt text", "A caption"));
  document.blocks.push_back(horizontal_rule());
  document.blocks.push_back(page_break());
  document.blocks.push_back(paragraph("end"));

  CHECK(document.blocks[0].is<ir::Heading>());
  CHECK(document.blocks[0].get_if<ir::Heading>()->level == 1);
  CHECK(document.blocks[1].get_if<ir::Paragraph>()->content.size() == 8);
  CHECK(document.blocks[6].get_if<ir::TableBlock>()->rows[0].header);
  CHECK(ir::plain_text(document.blocks[1].get_if<ir::Paragraph>()->content) ==
        "Plain, bold and nested code, a link\nnext line");
  CHECK(ir::plain_text(document) ==
        "Title\n\n"
        "Plain, bold and nested code, a link\nnext line\n\n"
        "- first\n- second\n\n  more\n\n"
        "9. one\n10. two\n\n"
        "> quoted\n>\n> twice\n\n"
        "int x;\nint y;\n\n"
        "h1\th2\na\tb\n\n"
        "A caption\n\n"
        "----------------------------------------\n\n"
        "end\n");
  CHECK(ir::plain_text(ir::Document{}).empty());

  // Copies are deep and independent.
  ir::Document copy = document;
  copy.blocks[0].get_if<ir::Heading>()->level = 3;
  CHECK(document.blocks[0].get_if<ir::Heading>()->level == 1);
}

TEST_CASE("engine: limits") {
  Limits limits;
  CHECK(limits.max_output_bytes == 2ull * 1024 * 1024 * 1024);
  CHECK(limits.max_pixels == 268435456ull);
  CHECK_OK(limits.check_pixels(16384, 16384));
  CHECK_STATUS(limits.check_pixels(16385, 16384), Status::Limit);
  CHECK_STATUS(limits.check_pixels(0xFFFFFFFFu, 0xFFFFFFFFu), Status::Limit);
  CHECK_STATUS(limits.check_pixels(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull), Status::Limit);  // no overflow
  CHECK_OK(limits.check_pixels(1000, 1000, 268));
  CHECK_STATUS(limits.check_pixels(1000, 1000, 269), Status::Limit);
  CHECK_OK(limits.check_pixels(0, 5));
  CHECK_OK(limits.check_input(limits.max_input_bytes));
  CHECK_STATUS(limits.check_input(limits.max_input_bytes + 1), Status::Limit);
  CHECK_STATUS(limits.check_output(limits.max_output_bytes + 1), Status::Limit);
  CHECK_STATUS(limits.check_archive_entries(limits.max_archive_entries + 1), Status::Limit);
  CHECK_STATUS(limits.check_decompressed(limits.max_decompressed_bytes + 1), Status::Limit);
}

TEST_CASE("engine: progress ranges, monotonicity and cancellation") {
  std::vector<std::pair<double, std::string>> seen;
  Progress progress([&seen](double fraction, std::string_view stage) {
    seen.emplace_back(fraction, std::string(stage));
    return fraction >= 0.9;
  });
  CHECK(progress.report(0.0, "Start"));
  CHECK(progress.report(0.25));
  const Progress half = progress.sub(0.5, 1.0, "Second half");
  CHECK(half.report(0.0));          // 0.5 overall
  CHECK(half.report(0.0001));       // too small a step to be worth a callback
  CHECK(progress.report(0.1));      // never runs backwards
  CHECK_FALSE(progress.cancelled());
  CHECK_FALSE(half.report(0.9));    // 0.95 overall: the sink asks to stop
  CHECK(progress.cancelled());
  CHECK(half.cancelled());
  CHECK_FALSE(progress.report(1.0));
  REQUIRE(seen.size() == 4);  // the tiny step and the backwards report were not worth a callback
  CHECK(seen[0] == std::pair<double, std::string>{0.0, "Start"});
  CHECK(seen[1] == std::pair<double, std::string>{0.25, "Start"});
  CHECK(seen[2] == std::pair<double, std::string>{0.5, "Second half"});
  CHECK(seen[3] == std::pair<double, std::string>{0.95, "Second half"});

  const Progress silent;
  CHECK(silent.report(0.5, "ignored"));
  CHECK_FALSE(silent.cancelled());
  silent.cancel();  // nothing to cancel
  CHECK_FALSE(silent.cancelled());

  Context ctx;
  CHECK_OK(ctx.checkpoint(0.5));
  ctx.progress = progress;
  CHECK_STATUS(ctx.checkpoint(0.5), Status::Cancelled);
}

TEST_CASE("engine: warnings are shared, de-duplicated and capped") {
  Context a;
  Context b;
  b.warnings = a.warnings;
  a.warn("first");
  b.warn("second");
  b.warn("first");
  CHECK(a.warnings.list() == std::vector<std::string>{"first", "second"});
  for (int i = 0; i < 500; ++i) a.warn("warning " + std::to_string(i));
  CHECK(a.warnings.list().size() == Warnings::max_messages);
  CHECK(Warnings().empty());
}

namespace {
Result<int> half(int value) {
  if (value % 2 != 0) return fail(Status::InvalidArgument, "{} is odd", value);
  return value / 2;
}
Result<int> quarter(int value) {
  CVT_TRY_ASSIGN(const int once, half(value));
  CVT_TRY_ASSIGN(int twice, half(once));
  CVT_TRY(half(twice * 2));
  return twice;
}
}  // namespace

TEST_CASE("engine: error helpers") {
  CHECK(quarter(8).value() == 2);
  CHECK(quarter(6).error().message == "3 is odd");
  CHECK(quarter(7).error().code == Status::InvalidArgument);
  const Result<void> plain = fail(Status::Io, "literal {} braces stay");
  CHECK(plain.error().message == "literal {} braces stay");
  const Result<void> rewrapped = fail(Error{Status::Limit, "too big"});
  CHECK(rewrapped.error().code == Status::Limit);
  CHECK(status_name(Status::Decode) == "decode error");
  static_assert(static_cast<int>(Status::Limit) == 8 && static_cast<int>(Status::Internal) == 9);
}

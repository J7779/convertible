// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>

#include "engine/convert.hpp"
#include "engine/fakes.hpp"
#include "util/fs.hpp"

using namespace cvt;
using test::fake_bridge;
using test::fake_codec;
using test::fake_format;

namespace {

// txt (PlainText, magic "TXT:") -> [upper-casing bridge to Document... no: PlainText->Table] -> out.
// A small but complete pipeline whose steps record what they were given.
struct Pipeline {
  Registry registry;
  std::vector<std::string> events;

  Pipeline() {
    FormatInfo text = fake_format("text", Category::Document, 50);
    text.signatures = {Signature::at(0, "'TXT:'")};
    registry.add_format(text);

    Codec reader;
    reader.format_id = "text";
    reader.kind = Kind::PlainText;
    reader.read_options = {OptionSpec::boolean("trim", "Trim", false)};
    reader.read = [this](ReadContext& ctx) -> Result<AnyIR> {
      events.push_back("read " + ctx.format_id + " name=" + ctx.filename + " trim=" +
                       (ctx.options.get_bool("trim", true) ? "1" : "0"));
      CVT_TRY(ctx.checkpoint(0.5, "Parsing"));
      if (as_chars(ctx.input).find("BROKEN") != std::string_view::npos) return fail(Status::Decode, "broken input");
      if (as_chars(ctx.input).find("THROW") != std::string_view::npos) throw std::runtime_error("reader exploded");
      if (as_chars(ctx.input).find("NOMEM") != std::string_view::npos) throw std::bad_alloc();
      if (as_chars(ctx.input).find("WRONGKIND") != std::string_view::npos) return ir::Image{};
      if (as_chars(ctx.input).find("WARN") != std::string_view::npos) ctx.warn("the reader noticed something");
      ir::PlainText out;
      out.text = std::string(as_chars(ctx.input).substr(4));
      return out;
    };
    registry.add_codec(std::move(reader));

    Bridge shout = fake_bridge(Kind::PlainText, Kind::Document, 10, false, "shout");
    shout.options = {OptionSpec::integer("repeat", "Repeat", 1, 1, 3)};
    shout.fn = [this](AnyIR value, Context& ctx) -> Result<AnyIR> {
      events.push_back("bridge repeat=" + std::to_string(ctx.options.get_int("repeat", 0)));
      CVT_TRY(ctx.checkpoint(0.5));
      ir::Document document;
      for (std::int64_t i = 0; i < ctx.options.get_int("repeat", 1); ++i) {
        document.blocks.push_back(ir::build::paragraph(value.get<ir::PlainText>().text));
      }
      return document;
    };
    registry.add_bridge(std::move(shout));

    Codec writer;
    writer.format_id = "page";
    writer.kind = Kind::Document;
    writer.write_options = {OptionSpec::integer("width", "Width", 80, 10, 200), OptionSpec::text("footer", "Footer", "")};
    writer.write = [this](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      events.push_back("write " + ctx.format_id + " from=" + ctx.source_format_id + " name=" + ctx.source_filename +
                       " width=" + std::to_string(ctx.options.get_int("width", 0)));
      CVT_TRY(ctx.checkpoint(0.5, "Laying out"));
      if (ctx.registry == nullptr) return fail(Status::Internal, "no registry in context");
      const std::string body = ir::plain_text(value.get<ir::Document>());
      if (body.find("UNWRITABLE") != std::string::npos) return fail(Status::Encode, "cannot be written");
      return to_bytes("<page>" + body + ctx.options.get_string("footer") + "</page>");
    };
    registry.add_codec(std::move(writer));
    FormatInfo page = fake_format("page", Category::Document, 40, {"pg", "page"});
    page.mime = "application/x-page";
    registry.add_format(page);

    registry.add_codec(fake_codec("dump", Kind::Blob, false, true));
    registry.add_codec(fake_codec("silent", Kind::Audio, true, true));
    registry.finalize();
  }

  Result<ConvertOutput> run(std::string_view input, std::string_view target = "page", std::string_view options = {},
                            std::string_view source = {}, std::string_view filename = {}, Progress progress = {},
                            Limits limits = {}) {
    events.clear();
    ConvertRequest request;
    request.input = as_bytes(input);
    request.filename = std::string(filename);
    request.source_id = std::string(source);
    request.target_id = std::string(target);
    request.options = Options::parse(options).value();
    request.progress = std::move(progress);
    request.limits = limits;
    return run_conversion(registry, request);
  }
};

}  // namespace

TEST_CASE("convert: a full route passes context, options and results along") {
  Pipeline p;
  REQUIRE(p.registry.validate().empty());
  const auto result = p.run("TXT:hello", "page", R"({"repeat":2,"width":"500","footer":"!"})", "", "dir/notes.text");
  REQUIRE_OK(result);
  CHECK(test::str(result->data) == "<page>hello\n\nhello\n!</page>");
  CHECK(result->source_format_id == "text");
  CHECK(result->format_id == "page");
  CHECK(result->extension == "pg");
  CHECK(result->mime == "application/x-page");
  CHECK(result->warnings.empty());
  CHECK(p.events == std::vector<std::string>{"read text name=notes.text trim=0", "bridge repeat=2",
                                             "write page from=text name=notes.text width=200"});

  // Format ids are forgiving (case, dot, extension); the mime type has a default.
  const auto by_extension = p.run("TXT:x", ".PG");
  REQUIRE_OK(by_extension);
  CHECK(by_extension->format_id == "page");
  const auto dump = p.run("TXT:x", "dump");
  REQUIRE_OK(dump);
  CHECK(test::str(dump->data) == "dump:Blob:TXT:x");  // implicit Blob reader: the raw bytes
  CHECK(dump->mime == "application/octet-stream");
  CHECK(dump->extension == "dump");
}

TEST_CASE("convert: argument and planning errors") {
  Pipeline p;
  CHECK_STATUS(p.run("TXT:x", ""), Status::InvalidArgument);
  CHECK_STATUS(p.run("TXT:x", "   "), Status::InvalidArgument);
  CHECK_STATUS(p.run("TXT:x", "nope"), Status::UnknownFormat);
  CHECK_STATUS(p.run("TXT:x", "page", "", "nope"), Status::UnknownFormat);
  CHECK_STATUS(p.run("\x01\x02\x03", "page"), Status::UnknownFormat);  // detection fails
  CHECK_STATUS(p.run("TXT:x", "silent"), Status::UnsupportedConversion);
  CHECK_STATUS(p.run("TXT:x", "page", R"({"repeat":"many"})"), Status::InvalidArgument);
  CHECK(p.events.size() == 1);  // the reader ran, the bridge's options were refused
  // An explicit source format skips detection.
  CHECK_OK(p.run("no magic here", "page", "", "text"));
}

TEST_CASE("convert: codec failures become errors, including exceptions and wrong kinds") {
  Pipeline p;
  CHECK_STATUS(p.run("TXT:BROKEN"), Status::Decode);
  CHECK_STATUS(p.run("TXT:UNWRITABLE"), Status::Encode);
  const auto thrown = p.run("TXT:THROW");
  CHECK_STATUS(thrown, Status::Internal);
  CHECK(thrown.error().message.find("reader exploded") != std::string::npos);
  CHECK_STATUS(p.run("TXT:NOMEM"), Status::Limit);
  const auto wrong = p.run("TXT:WRONGKIND");
  CHECK_STATUS(wrong, Status::Internal);
  CHECK(wrong.error().message.find("Image instead of PlainText") != std::string::npos);
}

TEST_CASE("convert: warnings are collected, unknown options are pointed out") {
  Pipeline p;
  const auto result = p.run("TXT:WARN", "page", R"({"wdith":100,"limits":{"max_pixels":10}})");
  REQUIRE_OK(result);
  CHECK(result->warnings == std::vector<std::string>{"option 'wdith' is not used by this conversion",
                                                     "the reader noticed something"});
}

TEST_CASE("convert: limits apply to input and output and can be overridden per call") {
  Pipeline p;
  Limits tight;
  tight.max_input_bytes = 8;
  CHECK_OK(p.run("TXT:1234", "page", "", "", "", {}, tight));
  CHECK_STATUS(p.run("TXT:12345", "page", "", "", "", {}, tight), Status::Limit);
  CHECK(p.events.empty());  // refused before any codec ran
  CHECK_OK(p.run("TXT:12345", "page", R"({"limits":{"max_input_bytes":100}})", "", "", {}, tight));
  CHECK_STATUS(p.run("TXT:x", "page", R"({"limits":{"max_output_bytes":5}})"), Status::Limit);
  CHECK_STATUS(p.run("TXT:x", "page", R"({"limits":{"max_bananas":5}})"), Status::InvalidArgument);
  CHECK_STATUS(p.run("TXT:x", "page", R"({"limits":{"max_pixels":-1}})"), Status::InvalidArgument);
  CHECK_STATUS(p.run("TXT:x", "page", R"({"limits":{"max_pixels":"many"}})"), Status::InvalidArgument);
  CHECK_STATUS(p.run("TXT:x", "page", R"({"limits":5})"), Status::InvalidArgument);

  const auto limits = effective_limits(Limits{}, Options::parse(
      R"({"limits":{"max_pixels":1000,"max_archive_entries":7,"max_decompressed_bytes":1e3,"max_output_bytes":18446744073709551615}})").value());
  REQUIRE_OK(limits);
  CHECK(limits->max_pixels == 1000);
  CHECK(limits->max_archive_entries == 7);
  CHECK(limits->max_decompressed_bytes == 1000);
  CHECK(limits->max_output_bytes == std::numeric_limits<std::uint64_t>::max());
  CHECK(limits->max_input_bytes == Limits{}.max_input_bytes);
}

TEST_CASE("convert: progress is monotonic, staged, and ends at 1") {
  Pipeline p;
  std::vector<std::pair<double, std::string>> seen;
  const auto result = p.run("TXT:x", "page", "", "", "", Progress([&seen](double fraction, std::string_view stage) {
                              seen.emplace_back(fraction, std::string(stage));
                              return false;
                            }));
  REQUIRE_OK(result);
  REQUIRE(seen.size() >= 6);
  CHECK(seen.front() == std::pair<double, std::string>{0.0, "Reading"});
  CHECK(seen.back() == std::pair<double, std::string>{1.0, "Done"});
  for (std::size_t i = 1; i < seen.size(); ++i) CHECK(seen[i - 1].first <= seen[i].first);
  std::vector<std::string> stages;
  for (const auto& [fraction, stage] : seen) {
    if (stages.empty() || stages.back() != stage) stages.push_back(stage);
  }
  CHECK(stages == std::vector<std::string>{"Reading", "Parsing", "Converting", "Writing", "Laying out", "Done"});
}

TEST_CASE("convert: cancelling at any point stops with Status::Cancelled") {
  Pipeline p;
  // Count the callbacks of an uninterrupted run, then cancel at each one in turn.
  int total = 0;
  REQUIRE_OK(p.run("TXT:x", "page", "", "", "", Progress([&total](double, std::string_view) {
    ++total;
    return false;
  })));
  REQUIRE(total >= 6);
  for (int cancel_at = 1; cancel_at <= total; ++cancel_at) {
    int calls = 0;
    const auto result = p.run("TXT:x", "page", "", "", "", Progress([&](double, std::string_view) {
      return ++calls >= cancel_at;
    }));
    CAPTURE(cancel_at);
    if (cancel_at == total) {
      // Asking to stop when the work is already complete still returns the result.
      CHECK(result.has_value());
    } else {
      CHECK_STATUS(result, Status::Cancelled);
    }
  }
  // A codec error raised after the user cancelled is reported as a cancellation.
  int calls = 0;
  CHECK_STATUS(p.run("TXT:BROKEN", "page", "", "", "", Progress([&calls](double, std::string_view) { return ++calls >= 2; })),
               Status::Cancelled);
}

TEST_CASE("convert: a streaming Video is pulled by the writer while the input is still alive") {
  Registry registry;
  Codec reader;
  reader.format_id = "frames";
  reader.kind = Kind::Video;
  reader.read = [](ReadContext& ctx) -> Result<AnyIR> {
    ir::Video video;
    video.info.width = 1;
    video.info.height = 1;
    // The stream keeps a view of the input, not a copy: one frame per input byte.
    auto position = std::make_shared<std::size_t>(0);
    const ByteView input = ctx.input;
    const Progress progress = ctx.progress;
    video.next_frame = [input, position, progress]() -> Result<std::optional<ir::VideoFrame>> {
      if (progress.cancelled()) return fail(Status::Cancelled, "cancelled");
      if (*position == input.size()) return std::nullopt;
      ir::VideoFrame frame = ir::VideoFrame::allocate(ir::PixelFormat::RGBA8, 1, 1);
      frame.planes[0].data[0] = std::to_integer<std::uint8_t>(input[*position]);
      frame.pts_ms = static_cast<std::int64_t>((*position)++) * 40;
      return frame;
    };
    return video;
  };
  registry.add_codec(std::move(reader));
  Bridge pass;
  pass.from = Kind::Video;
  pass.to = Kind::PlainText;
  pass.label = "frames as text";
  pass.fn = [](AnyIR value, Context&) -> Result<AnyIR> {
    ir::Video video = std::move(value).get<ir::Video>();
    ir::PlainText text;
    while (true) {
      CVT_TRY_ASSIGN(std::optional<ir::VideoFrame> frame, video.next_frame());
      if (!frame) break;
      text.text.push_back(static_cast<char>(frame->planes[0].data[0]));
    }
    return text;
  };
  registry.add_bridge(std::move(pass));
  Codec writer;
  writer.format_id = "frames";
  writer.kind = Kind::Video;
  writer.write = [](const AnyIR& value, WriteContext&) -> Result<Bytes> {
    const ir::Video& video = value.get<ir::Video>();
    Bytes out;
    while (true) {
      CVT_TRY_ASSIGN(const std::optional<ir::VideoFrame> frame, video.next_frame());
      if (!frame) break;
      out.push_back(static_cast<std::byte>(frame->planes[0].data[0] ^ 0x20));  // "re-encode": flip case
    }
    return out;
  };
  registry.add_codec(std::move(writer));
  registry.add_codec(fake_codec("text", Kind::PlainText, false, true));
  registry.finalize();
  REQUIRE(registry.validate().empty());

  ConvertRequest request;
  const std::string input = "streamed";
  request.input = as_bytes(input);
  request.source_id = "frames";
  request.target_id = "frames";  // same format: the "compress" route
  const auto recoded = run_conversion(registry, request);
  REQUIRE_OK(recoded);
  CHECK(test::str(recoded->data) == "STREAMED");
  request.target_id = "text";
  const auto bridged = run_conversion(registry, request);
  REQUIRE_OK(bridged);
  CHECK(test::str(bridged->data) == "text:PlainText:streamed");
}

TEST_CASE("convert: file to file") {
  Pipeline p;
  auto temp = util::TempDir::create("cvt-test");
  REQUIRE_OK(temp);
  const std::filesystem::path input = temp->path() / util::path_from_utf8("entr\xC3\xA9" "e.text");
  const std::filesystem::path output = temp->path() / "out.pg";
  REQUIRE_OK(util::write_file(input, as_bytes("TXT:from disk")));

  FileConvertRequest request;
  request.input = input;
  request.output = output;
  request.target_id = "page";
  const auto result = run_file_conversion(p.registry, request);
  REQUIRE_OK(result);
  CHECK(result->data.empty());
  CHECK(result->format_id == "page");
  CHECK(result->extension == "pg");
  CHECK(test::str(*util::read_file(output)) == "<page>from disk\n</page>");
  CHECK(p.events.front() == "read text name=entr\xC3\xA9" "e.text trim=0");

  // In place: the mapping of the input is released before the output is written.
  request.output = input;
  REQUIRE_OK(run_file_conversion(p.registry, request));
  CHECK(test::str(*util::read_file(input)) == "<page>from disk\n</page>");

  // Failures leave no output behind.
  REQUIRE_OK(util::write_file(input, as_bytes("TXT:BROKEN")));
  request.output = temp->path() / "never.pg";
  CHECK_STATUS(run_file_conversion(p.registry, request), Status::Decode);
  CHECK_FALSE(std::filesystem::exists(request.output));
  request.output = temp->path() / "missing-dir" / "x.pg";
  REQUIRE_OK(util::write_file(input, as_bytes("TXT:fine")));
  CHECK_STATUS(run_file_conversion(p.registry, request), Status::Io);
  request.output = output;
  request.input = temp->path() / "does-not-exist.text";
  CHECK_STATUS(run_file_conversion(p.registry, request), Status::Io);
  request.input = temp->path();
  CHECK_STATUS(run_file_conversion(p.registry, request), Status::Io);
  request.input.clear();
  CHECK_STATUS(run_file_conversion(p.registry, request), Status::InvalidArgument);
  request.input = input;
  request.output.clear();
  CHECK_STATUS(run_file_conversion(p.registry, request), Status::InvalidArgument);
  request.output = output;
  request.limits.max_input_bytes = 3;
  CHECK_STATUS(run_file_conversion(p.registry, request), Status::Limit);

  // An empty file is a valid input (it cannot be memory-mapped).
  REQUIRE_OK(util::write_file(input, {}));
  request.limits = {};
  request.source_id = "text";
  request.target_id = "dump";
  REQUIRE_OK(run_file_conversion(p.registry, request));
  CHECK(test::str(*util::read_file(output)) == "dump:Blob:");
}

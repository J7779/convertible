// Convertible core - image family: JPEG (jpg/jpeg/jpe/jfif), MPO and the
// "mjpeg" video codec.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "codec/codec.hpp"
#include "codec/colorspace.hpp"
#include "codec/ids.hpp"
#include "codec/registry.hpp"
#include "engine/registry.hpp"
#include "engine/standard_options.hpp"
#include "families/image/common.hpp"
#include "families/image/image.hpp"
#include "families/image/jpeg_codec.hpp"
#include "util/bytes.hpp"

namespace cvt::image {

namespace {

Result<ir::Image> read_jpeg(ReadContext& ctx) { return jpeg::decode(ctx.input, ctx); }

jpeg::Subsampling resolved_subsampling(const Options& values) {
  const std::string value = values.get_enum("chroma_subsampling", "auto", {"auto", "444", "422", "420"});
  if (value == "444") return jpeg::Subsampling::S444;
  if (value == "422") return jpeg::Subsampling::S422;
  if (value == "420") return jpeg::Subsampling::S420;
  return jpeg::Subsampling::Auto;
}

Result<Bytes> write_jpeg(const ir::Image& source, WriteContext& ctx) {
  CVT_TRY_ASSIGN(const WriteImage prepared, prepare_write(source, ctx));
  warn_if_animated(*prepared, ctx, "JPEG");
  CVT_TRY_ASSIGN(const ir::Frame frame, opaque_frame(prepared->frames.front(), ctx, "JPEG"));
  jpeg::EncodeOptions options;
  options.quality = options::resolved_quality(ctx.options);
  options.subsampling = resolved_subsampling(ctx.options);
  options.progressive = ctx.options.get_bool("progressive", false);
  options.grayscale = ctx.options.get_bool("grayscale", false);
  options.dpi_x = prepared->dpi_x;
  options.dpi_y = prepared->dpi_y;
  if (prepared.keep_metadata()) {
    options.exif = prepared->exif;
    options.icc = prepared->icc_profile;
    options.xmp = prepared->xmp;
    if (const auto it = prepared->metadata.find("Comment"); it != prepared->metadata.end()) options.comment = it->second;
  }
  return jpeg::encode(frame, options, ctx);
}

std::vector<OptionSpec> jpeg_write_options() {
  std::vector<OptionSpec> specs = lossy_writer_options();
  specs.push_back(OptionSpec::choice("chroma_subsampling", "Chroma subsampling", "auto", {"auto", "444", "422", "420"},
                                     "Resolution of the colour channels: 4:4:4 keeps full colour detail, 4:2:0 "
                                     "makes smaller files. Auto uses 4:4:4 from quality 90 up and 4:2:0 below.")
                      .advanced());
  specs.push_back(OptionSpec::boolean("progressive", "Progressive", false,
                                      "Store the picture in several passes so it appears quickly while loading; "
                                      "usually slightly smaller."));
  specs.push_back(OptionSpec::boolean("grayscale", "Greyscale", false, "Write a single grey channel.").advanced());
  specs.push_back(background_option());
  return specs;
}

/// True when an APP2 "MPF" segment appears before the first scan.
bool has_mpf(ByteView data) {
  util::ByteReader reader(data);
  if (reader.u8() != 0xFF || reader.u8() != 0xD8) return false;
  for (int i = 0; i < 64 && reader.ok(); ++i) {
    if (reader.u8() != 0xFF) return false;
    std::uint8_t marker = reader.u8();
    while (marker == 0xFF && reader.ok()) marker = reader.u8();
    if (marker == 0xDA || marker == 0xD9) return false;
    const std::uint16_t length = reader.u16be();
    if (length < 2) return false;
    const ByteView body = reader.bytes(length - 2u);
    if (!reader.ok()) return false;
    if (marker == 0xE2 && body.size() >= 4 && as_chars(body.first(4)) == std::string_view("MPF\0", 4)) return true;
  }
  return false;
}

int sniff_mpo(std::span<const std::byte> data, std::string_view /*filename*/) { return has_mpf(data) ? 96 : 0; }

// --------------------------------------------------------------------------- mjpeg video codec

constexpr codec::ColorInfo jpeg_color{ir::ColorMatrix::BT601, ir::ColorRange::Full, ir::ChromaSiting::Center};

class MjpegDecoder final : public codec::VideoDecoder {
 public:
  Result<void> configure(const codec::CodecParams& params) override {
    timescale_ = params.timescale == 0 ? 1000 : params.timescale;
    return {};
  }
  Result<void> send_packet(const codec::Packet& packet) override {
    Context ctx;
    jpeg::DecodeOptions options;
    options.default_huffman = true;  // AVI MJPEG omits the DHT segment
    CVT_TRY_ASSIGN(const jpeg::Planes planes, jpeg::decode_planes(packet.bytes, ctx, options));
    CVT_TRY_ASSIGN(codec::VideoFrame frame, to_video_frame(planes, ctx));
    frame.pts_ms = packet.pts * 1000 / static_cast<std::int64_t>(timescale_);
    frame.duration_ms = packet.duration * 1000 / static_cast<std::int64_t>(timescale_);
    pending_ = std::move(frame);
    return {};
  }
  Result<std::optional<codec::VideoFrame>> receive_frame() override {
    std::optional<codec::VideoFrame> out = std::move(pending_);
    pending_.reset();
    return out;
  }
  Result<void> flush() override { return {}; }

 private:
  static Result<codec::VideoFrame> to_video_frame(const jpeg::Planes& planes, const Context& ctx) {
    const auto& comps = planes.components;
    if (planes.precision == 8 && planes.model == jpeg::ColorModel::YCbCr && comps.size() == 3 && comps[1].h == comps[2].h &&
        comps[1].v == comps[2].v && comps[1].h == 1 && comps[1].v == 1) {
      codec::PixelFormat format = codec::PixelFormat::RGBA8;
      if (comps[0].h == 2 && comps[0].v == 2) format = codec::PixelFormat::I420;
      if (comps[0].h == 2 && comps[0].v == 1) format = codec::PixelFormat::I422;
      if (comps[0].h == 1 && comps[0].v == 1) format = codec::PixelFormat::I444;
      if (format != codec::PixelFormat::RGBA8) {
        codec::VideoFrame frame = codec::VideoFrame::allocate(format, planes.width, planes.height);
        frame.color = jpeg_color;
        for (std::size_t p = 0; p < 3; ++p) {
          const jpeg::Plane& src = comps[p];
          const std::uint32_t rows = ir::plane_height(format, p, planes.height);
          const std::uint32_t cols = ir::plane_width(format, p, planes.width);
          for (std::uint32_t y = 0; y < rows; ++y) {
            const std::uint32_t sy = std::min(y, src.height - 1);
            std::memcpy(frame.planes[p].data.data() + y * frame.planes[p].stride,
                        src.samples.data() + static_cast<std::size_t>(sy) * src.width, std::min(cols, src.width));
          }
        }
        return frame;
      }
    }
    if (planes.precision == 8 && planes.model == jpeg::ColorModel::Gray) {
      codec::VideoFrame frame = codec::VideoFrame::allocate(codec::PixelFormat::I420, planes.width, planes.height);
      frame.color = jpeg_color;
      const jpeg::Plane& src = comps[0];
      for (std::uint32_t y = 0; y < planes.height; ++y) {
        std::memcpy(frame.planes[0].data.data() + y * frame.planes[0].stride,
                    src.samples.data() + static_cast<std::size_t>(y) * src.width, planes.width);
      }
      std::fill(frame.planes[1].data.begin(), frame.planes[1].data.end(), std::uint8_t{128});
      std::fill(frame.planes[2].data.begin(), frame.planes[2].data.end(), std::uint8_t{128});
      return frame;
    }
    // Other layouts (4:1:1, 4:4:0, RGB, CMYK, 12 bit): through RGBA.
    CVT_TRY_ASSIGN(ir::Frame rgba, jpeg::planes_to_frame(planes, ctx));
    codec::VideoFrame frame;
    frame.width = rgba.width;
    frame.height = rgba.height;
    frame.format = codec::PixelFormat::RGBA8;
    frame.planes.resize(1);
    frame.planes[0].stride = static_cast<std::size_t>(rgba.width) * 4;
    frame.planes[0].data = std::move(rgba.rgba);
    return codec::from_rgba(frame, codec::PixelFormat::I420, jpeg_color);
  }

  std::uint32_t timescale_ = 1000;
  std::optional<codec::VideoFrame> pending_;
};

class MjpegEncoder final : public codec::VideoEncoder {
 public:
  Result<void> configure(const codec::VideoEncodeParams& params) override {
    if (params.width == 0 || params.height == 0 || params.width > 65535 || params.height > 65535) {
      return fail(Status::InvalidArgument, "Motion JPEG needs a frame size between 1 and 65535 pixels");
    }
    params_ = params;
    if (params_.timescale == 0) params_.timescale = 90000;
    return {};
  }
  Result<void> send_frame(const codec::VideoFrame& frame) override {
    Context ctx;
    jpeg::EncodeOptions options;
    options.quality = std::clamp(params_.quality, 1, 100);
    options.optimize = params_.preset != codec::EncodePreset::Fast;
    Bytes bytes;
    if (frame.format == codec::PixelFormat::RGBA8) {
      CVT_TRY_ASSIGN(const codec::VideoFrame packed, codec::to_rgba(frame));
      ir::Frame f;
      f.width = packed.width;
      f.height = packed.height;
      f.rgba = packed.planes[0].data;
      f.rgba.resize(static_cast<std::size_t>(f.width) * f.height * 4);
      options.subsampling = jpeg::Subsampling::S420;
      CVT_TRY_ASSIGN(bytes, jpeg::encode(f, options, ctx));
    } else {
      codec::PixelFormat format = frame.format;
      if (format != codec::PixelFormat::I420 && format != codec::PixelFormat::I422 && format != codec::PixelFormat::I444) {
        format = codec::PixelFormat::I420;
      }
      const codec::VideoFrame* source = &frame;
      codec::VideoFrame converted;
      if (format != frame.format || !(frame.color == jpeg_color)) {
        CVT_TRY_ASSIGN(converted, codec::convert_frame(frame, format, jpeg_color));
        source = &converted;
      }
      if (!source->valid()) return fail(Status::InvalidArgument, "invalid video frame");
      jpeg::PlanarInput input;
      input.width = source->width;
      input.height = source->height;
      input.subsampling = format == codec::PixelFormat::I444   ? jpeg::Subsampling::S444
                          : format == codec::PixelFormat::I422 ? jpeg::Subsampling::S422
                                                               : jpeg::Subsampling::S420;
      input.y = source->planes[0].data.data();
      input.y_stride = source->planes[0].stride;
      input.cb = source->planes[1].data.data();
      input.cb_stride = source->planes[1].stride;
      input.cr = source->planes[2].data.data();
      input.cr_stride = source->planes[2].stride;
      CVT_TRY_ASSIGN(bytes, jpeg::encode_planes(input, options, ctx));
    }
    codec::Packet packet;
    packet.bytes = std::move(bytes);
    packet.pts = frame.pts_ms * static_cast<std::int64_t>(params_.timescale) / 1000;
    packet.dts = packet.pts;
    packet.duration = frame.duration_ms * static_cast<std::int64_t>(params_.timescale) / 1000;
    packet.keyframe = true;
    pending_.push_back(std::move(packet));
    return {};
  }
  Result<std::optional<codec::Packet>> receive_packet() override {
    if (pending_.empty()) return std::optional<codec::Packet>{};
    codec::Packet packet = std::move(pending_.front());
    pending_.erase(pending_.begin());
    return std::optional<codec::Packet>(std::move(packet));
  }
  Bytes extradata() const override { return {}; }
  Result<void> flush() override { return {}; }

 private:
  codec::VideoEncodeParams params_;
  std::vector<codec::Packet> pending_;
};

}  // namespace

void register_jpeg(Registry& registry) {
  FormatInfo info;
  info.id = "jpg";
  info.name = "JPEG image";
  info.description = "Lossy compressed photograph (JFIF/EXIF).";
  info.category = Category::Image;
  info.extensions = {"jpg", "jpeg", "jpe", "jfif", "jif"};
  info.mime = "image/jpeg";
  info.popularity = 95;
  info.signatures = {Signature::at(0, "FF D8 FF")};
  registry.add_format(std::move(info));

  FormatInfo mpo;
  mpo.id = "mpo";
  mpo.name = "Multi-picture JPEG (MPO)";
  mpo.description = "Stereo or multi-view JPEG from 3D cameras; the first picture is read.";
  mpo.category = Category::Image;
  mpo.extensions = {"mpo"};
  mpo.mime = "image/mpo";
  mpo.popularity = 10;
  mpo.sniff = sniff_mpo;
  registry.add_format(std::move(mpo));

  ImageCodecSpec spec;
  spec.format_id = "jpg";
  spec.read = reader(read_jpeg);
  spec.write = writer(write_jpeg);
  spec.write_options = jpeg_write_options();
  spec.lossy_write = true;
  spec.write_cost = 12;
  add_image_codec(registry, std::move(spec));

  ImageCodecSpec mpo_spec;
  mpo_spec.format_id = "mpo";
  mpo_spec.read = reader(read_jpeg);
  add_image_codec(registry, std::move(mpo_spec));

  registry.media().register_video_decoder(codec::id::mjpeg, [] { return std::make_unique<MjpegDecoder>(); });
  registry.media().register_video_encoder(codec::id::mjpeg, [] { return std::make_unique<MjpegEncoder>(); });
}

}  // namespace cvt::image

// Convertible core - image_modern family: the OpenJPEG wrapper.
//
// The only file that includes the imported library. It moves codestreams
// through memory streams (never files), keeps OpenJPEG single-threaded,
// forwards its warnings into the conversion's warnings and its last error
// into the error message, and turns cancellation requests seen in the stream
// callbacks into Status::Cancelled. Callers check the limits before calling
// in: by the time OpenJPEG allocates, the SIZ geometry has been validated
// in-house (jp2_box.cpp) and accepted by `Limits`.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "util/bytes.hpp"
#include "families/image_modern/jp2_codec.hpp"
#include "openjp2/openjpeg.h"

namespace cvt::image_modern::jp2 {

namespace {

/// Messages reported by OpenJPEG during one call.
struct Messages {
  const Context* ctx = nullptr;
  std::string last_error;
  int warnings = 0;
};

std::string trimmed(const char* text) {
  std::string s = text ? text : "";
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

void on_error(const char* text, void* user) {
  static_cast<Messages*>(user)->last_error = trimmed(text);
}

void on_warning(const char* text, void* user) {
  auto* m = static_cast<Messages*>(user);
  // OpenJPEG can repeat a warning per tile or packet; a few are enough.
  if (m->ctx != nullptr && m->warnings < 4) {
    ++m->warnings;
    m->ctx->warn("JPEG 2000: " + trimmed(text));
  }
}

void on_info(const char* /*text*/, void* /*user*/) {}

void install_handlers(opj_codec_t* codec, Messages& messages) {
  opj_set_error_handler(codec, on_error, &messages);
  opj_set_warning_handler(codec, on_warning, &messages);
  opj_set_info_handler(codec, on_info, &messages);
}

struct CodecDeleter {
  void operator()(opj_codec_t* codec) const noexcept { opj_destroy_codec(codec); }
};
struct StreamDeleter {
  void operator()(opj_stream_t* stream) const noexcept { opj_stream_destroy(stream); }
};
struct ImageDeleter {
  void operator()(opj_image_t* image) const noexcept { opj_image_destroy(image); }
};
using CodecPtr = std::unique_ptr<opj_codec_t, CodecDeleter>;
using StreamPtr = std::unique_ptr<opj_stream_t, StreamDeleter>;
using ImagePtr = std::unique_ptr<opj_image_t, ImageDeleter>;

// --------------------------------------------------------------------------- input stream

struct InputStream {
  ByteView data;
  std::size_t position = 0;
  const Context* ctx = nullptr;
  bool cancelled = false;
};

OPJ_SIZE_T read_input(void* buffer, OPJ_SIZE_T count, void* user) {
  auto* in = static_cast<InputStream*>(user);
  if (in->cancelled || in->position >= in->data.size()) return static_cast<OPJ_SIZE_T>(-1);
  const std::size_t n = std::min<std::size_t>(count, in->data.size() - in->position);
  ::cvt::util::copy_bytes(buffer, in->data.data() + in->position, n);
  in->position += n;
  // Tile data is read tile by tile between decoding steps, so the read
  // position is a fair measure of progress and a place to notice a cancel.
  if (in->ctx != nullptr && !in->ctx->checkpoint(0.95 * static_cast<double>(in->position) / static_cast<double>(in->data.size()))) {
    in->cancelled = true;
  }
  return n;
}

OPJ_OFF_T skip_input(OPJ_OFF_T count, void* user) {
  auto* in = static_cast<InputStream*>(user);
  if (count >= 0) {
    if (in->position >= in->data.size()) return -1;
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(count),
                                                                     in->data.size() - in->position));
    in->position += n;
    return static_cast<OPJ_OFF_T>(n);
  }
  const auto back = static_cast<std::size_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(-count), in->position));
  in->position -= back;
  return -static_cast<OPJ_OFF_T>(back);
}

OPJ_BOOL seek_input(OPJ_OFF_T offset, void* user) {
  auto* in = static_cast<InputStream*>(user);
  if (offset < 0 || static_cast<std::uint64_t>(offset) > in->data.size()) return OPJ_FALSE;
  in->position = static_cast<std::size_t>(offset);
  return OPJ_TRUE;
}

// --------------------------------------------------------------------------- output stream

struct OutputStream {
  Bytes data;
  std::size_t position = 0;
  const Context* ctx = nullptr;
  bool failed = false;
};

OPJ_SIZE_T write_output(void* buffer, OPJ_SIZE_T count, void* user) {
  auto* out = static_cast<OutputStream*>(user);
  if (out->failed) return static_cast<OPJ_SIZE_T>(-1);
  const std::uint64_t end = std::uint64_t{out->position} + count;
  if (out->ctx != nullptr && !out->ctx->limits.check_output(end)) {
    out->failed = true;
    return static_cast<OPJ_SIZE_T>(-1);
  }
  if (end > out->data.size()) out->data.resize(static_cast<std::size_t>(end));
  ::cvt::util::copy_bytes(out->data.data() + out->position, buffer, count);
  out->position = static_cast<std::size_t>(end);
  return count;
}

OPJ_OFF_T skip_output(OPJ_OFF_T count, void* user) {
  auto* out = static_cast<OutputStream*>(user);
  const std::int64_t target = static_cast<std::int64_t>(out->position) + count;
  if (target < 0) return -1;
  if (static_cast<std::uint64_t>(target) > out->data.size()) out->data.resize(static_cast<std::size_t>(target));
  out->position = static_cast<std::size_t>(target);
  return count;
}

OPJ_BOOL seek_output(OPJ_OFF_T offset, void* user) {
  auto* out = static_cast<OutputStream*>(user);
  if (offset < 0) return OPJ_FALSE;
  if (static_cast<std::uint64_t>(offset) > out->data.size()) out->data.resize(static_cast<std::size_t>(offset));
  out->position = static_cast<std::size_t>(offset);
  return OPJ_TRUE;
}

std::string reason(const Messages& messages, std::string_view fallback) {
  return messages.last_error.empty() ? std::string(fallback) : messages.last_error;
}

OPJ_PROG_ORDER to_opj(Progression p) {
  switch (p) {
    case Progression::LRCP: return OPJ_LRCP;
    case Progression::RLCP: return OPJ_RLCP;
    case Progression::RPCL: return OPJ_RPCL;
    case Progression::PCRL: return OPJ_PCRL;
    case Progression::CPRL: return OPJ_CPRL;
  }
  return OPJ_LRCP;
}

}  // namespace

Result<DecodedPlanes> decode_codestream(ByteView codestream, const SizInfo& siz, const Context& ctx) {
  Messages messages;
  messages.ctx = &ctx;
  CodecPtr codec(opj_create_decompress(OPJ_CODEC_J2K));
  if (!codec) return fail(Status::Internal, "the JPEG 2000 decoder could not be created");
  install_handlers(codec.get(), messages);
  opj_dparameters_t parameters;
  opj_set_default_decoder_parameters(&parameters);
  if (!opj_setup_decoder(codec.get(), &parameters)) {
    return fail(Status::Internal, "the JPEG 2000 decoder could not be set up: {}", reason(messages, "unknown error"));
  }
  // Partial codestreams (truncated files) decode as far as the data goes.
  opj_decoder_set_strict_mode(codec.get(), OPJ_FALSE);

  InputStream input{codestream, 0, &ctx, false};
  StreamPtr stream(opj_stream_create(64 * 1024, OPJ_TRUE));
  if (!stream) return fail(Status::Internal, "the JPEG 2000 input stream could not be created");
  opj_stream_set_user_data(stream.get(), &input, nullptr);
  opj_stream_set_user_data_length(stream.get(), codestream.size());
  opj_stream_set_read_function(stream.get(), read_input);
  opj_stream_set_skip_function(stream.get(), skip_input);
  opj_stream_set_seek_function(stream.get(), seek_input);

  opj_image_t* raw_image = nullptr;
  const OPJ_BOOL header_ok = opj_read_header(stream.get(), codec.get(), &raw_image);
  ImagePtr image(raw_image);
  if (input.cancelled) return fail(Status::Cancelled, "the conversion was cancelled");
  if (!header_ok || !image) {
    return fail(Status::Decode, "the JPEG 2000 codestream header is invalid: {}", reason(messages, "unknown error"));
  }
  // The library must see the same geometry that the limits were checked against.
  if (image->x0 != siz.x0 || image->y0 != siz.y0 || image->x1 != siz.x1 || image->y1 != siz.y1 ||
      image->numcomps != siz.components.size()) {
    return fail(Status::Decode, "the JPEG 2000 codestream header is inconsistent");
  }
  const OPJ_BOOL decoded = opj_decode(codec.get(), stream.get(), image.get());
  if (input.cancelled) return fail(Status::Cancelled, "the conversion was cancelled");
  if (!decoded) {
    return fail(Status::Decode, "the JPEG 2000 codestream could not be decoded: {}",
                reason(messages, "unknown error"));
  }
  if (!opj_end_decompress(codec.get(), stream.get()) && !messages.last_error.empty()) {
    ctx.warn("JPEG 2000: " + messages.last_error);
  }

  DecodedPlanes out;
  out.x0 = image->x0;
  out.y0 = image->y0;
  out.x1 = image->x1;
  out.y1 = image->y1;
  for (OPJ_UINT32 i = 0; i < image->numcomps; ++i) {
    const opj_image_comp_t& c = image->comps[i];
    if (c.data == nullptr || c.w == 0 || c.h == 0 || c.dx == 0 || c.dy == 0 || c.prec == 0 || c.prec > 31) {
      return fail(Status::Decode, "the JPEG 2000 component {} could not be decoded", i);
    }
    Plane plane;
    plane.dx = c.dx;
    plane.dy = c.dy;
    plane.x0 = c.x0;
    plane.y0 = c.y0;
    plane.width = c.w;
    plane.height = c.h;
    plane.precision = c.prec;
    plane.is_signed = c.sgnd != 0;
    plane.data = c.data;
    out.planes.push_back(plane);
  }
  out.owner = std::shared_ptr<void>(image.release(), [](void* p) { opj_image_destroy(static_cast<opj_image_t*>(p)); });
  CVT_TRY(ctx.checkpoint(1.0));
  return out;
}

Result<Bytes> encode_codestream(const EncodeParams& params, const Context& ctx) {
  if (params.width == 0 || params.height == 0 || params.components == 0 || params.components > 16384 ||
      params.precision == 0 || params.precision > 16 || !params.fill) {
    return fail(Status::Encode, "invalid JPEG 2000 encoder input");
  }
  opj_cparameters_t p;
  opj_set_default_encoder_parameters(&p);
  const int layers = std::clamp(params.layers, 1, 20);
  p.tcp_numlayers = layers;
  p.cp_fixed_quality = 1;
  if (params.lossless) {
    p.irreversible = 0;
    // Earlier layers at rising PSNR between 30 and 50 dB, the last one lossless.
    for (int i = 0; i + 1 < layers; ++i) {
      p.tcp_distoratio[i] = static_cast<float>(30.0 + 20.0 * i / std::max(1, layers - 1));
    }
    p.tcp_distoratio[layers - 1] = 0.0f;
  } else {
    p.irreversible = 1;
    // Each earlier layer 4 dB below the next one (never below 15 dB).
    for (int i = 0; i < layers; ++i) {
      p.tcp_distoratio[i] = static_cast<float>(std::max(15.0 + i, params.psnr - 4.0 * (layers - 1 - i)));
    }
  }
  p.prog_order = to_opj(params.progression);
  std::uint32_t tile_w = params.width;
  std::uint32_t tile_h = params.height;
  if (params.tile_size > 0 && (params.tile_size < params.width || params.tile_size < params.height)) {
    p.tile_size_on = OPJ_TRUE;
    p.cp_tdx = static_cast<int>(params.tile_size);
    p.cp_tdy = static_cast<int>(params.tile_size);
    tile_w = std::min(tile_w, params.tile_size);
    tile_h = std::min(tile_h, params.tile_size);
  }
  // Up to five decomposition levels, fewer for tiny tiles (2^(levels) <= edge).
  int resolutions = 6;
  while (resolutions > 1 && (std::min(tile_w, tile_h) >> (resolutions - 1)) == 0) --resolutions;
  p.numresolution = resolutions;
  p.tcp_mct = (params.colour_transform && params.components >= 3) ? 1 : 0;

  std::vector<opj_image_cmptparm_t> parts(params.components);
  for (opj_image_cmptparm_t& c : parts) {
    ::cvt::util::fill_bytes(&c, 0, sizeof c);
    c.dx = 1;
    c.dy = 1;
    c.w = params.width;
    c.h = params.height;
    c.prec = params.precision;
    c.sgnd = 0;
  }
  const OPJ_COLOR_SPACE space = params.components >= 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY;
  ImagePtr image(opj_image_create(params.components, parts.data(), space));
  if (!image) return fail(Status::Limit, "not enough memory for the JPEG 2000 encoder");
  image->x0 = 0;
  image->y0 = 0;
  image->x1 = params.width;
  image->y1 = params.height;
  for (std::uint32_t i = 0; i < params.components; ++i) params.fill(i, image->comps[i].data);
  CVT_TRY(ctx.checkpoint(0.1));

  Messages messages;
  messages.ctx = &ctx;
  CodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
  if (!codec) return fail(Status::Internal, "the JPEG 2000 encoder could not be created");
  install_handlers(codec.get(), messages);
  if (!opj_setup_encoder(codec.get(), &p, image.get())) {
    return fail(Status::Encode, "the JPEG 2000 encoder rejected the settings: {}", reason(messages, "unknown error"));
  }
  OutputStream output;
  output.ctx = &ctx;
  output.data.reserve(static_cast<std::size_t>(
      std::min<std::uint64_t>(std::uint64_t{params.width} * params.height * params.components / 2 + 1024, 64u << 20)));
  StreamPtr stream(opj_stream_create(64 * 1024, OPJ_FALSE));
  if (!stream) return fail(Status::Internal, "the JPEG 2000 output stream could not be created");
  opj_stream_set_user_data(stream.get(), &output, nullptr);
  opj_stream_set_write_function(stream.get(), write_output);
  opj_stream_set_skip_function(stream.get(), skip_output);
  opj_stream_set_seek_function(stream.get(), seek_output);

  const bool ok = opj_start_compress(codec.get(), image.get(), stream.get()) &&
                  opj_encode(codec.get(), stream.get()) && opj_end_compress(codec.get(), stream.get());
  stream.reset();  // flushes nothing further; the buffer is ours
  if (output.failed) {
    return fail(Status::Limit, "the JPEG 2000 output exceeds the limit of {} bytes", ctx.limits.max_output_bytes);
  }
  if (!ok) return fail(Status::Encode, "JPEG 2000 encoding failed: {}", reason(messages, "unknown error"));
  output.data.resize(output.position);
  CVT_TRY(ctx.checkpoint(1.0));
  return std::move(output.data);
}

}  // namespace cvt::image_modern::jp2

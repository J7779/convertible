// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/encoding/simple.hpp"

#include <cstdint>
#include <utility>

#include "util/text.hpp"

namespace cvt::encoding {

namespace {

FormatInfo describe(std::string id, std::string name, std::string description, std::vector<std::string> extensions,
                    std::string mime, int popularity, std::function<int(ByteView, std::string_view)> sniff) {
  FormatInfo info;
  info.id = std::move(id);
  info.name = std::move(name);
  info.description = std::move(description);
  info.category = Category::Encoding;
  info.extensions = std::move(extensions);
  info.mime = std::move(mime);
  info.popularity = popularity;
  info.sniff = std::move(sniff);
  return info;
}

}  // namespace

std::string input_as_text(ByteView input) {
  const util::EncodingGuess guess = util::guess_encoding(input);
  if (guess.encoding == util::TextEncoding::Utf8 || guess.encoding == util::TextEncoding::Windows1252) {
    // Text encodings are ASCII; anything else is the decoder's to reject.
    return std::string(as_chars(input.subspan(guess.bom_length)));
  }
  return util::decode_text(input, guess.encoding).text;
}

void add_binary_encoding(Registry& registry, BinaryEncoding encoding) {
  registry.add_format(describe(encoding.id, std::move(encoding.name), std::move(encoding.description),
                               std::move(encoding.extensions), std::move(encoding.mime), encoding.popularity,
                               std::move(encoding.sniff)));

  Codec codec;
  codec.format_id = encoding.id;
  codec.kind = Kind::Blob;
  codec.write_options = std::move(encoding.encode_options);
  codec.read_options = std::move(encoding.decode_options);

  if (encoding.encode) {
    codec.write = [encode = std::move(encoding.encode), expansion = encoding.expansion](
                      const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      const ir::Blob& blob = value.get<ir::Blob>();
      if (expansion != 0 && blob.bytes.size() > ctx.limits.max_output_bytes / expansion) {
        return fail(Status::Limit, "the encoded output would exceed the limit of {} bytes", ctx.limits.max_output_bytes);
      }
      CVT_TRY_ASSIGN(const std::string text, encode(blob, ctx));
      CVT_TRY(ctx.limits.check_output(text.size()));
      return to_bytes(text);
    };
  }

  if (encoding.decode || encoding.decode_blob) {
    codec.read = [decode = std::move(encoding.decode), decode_blob = std::move(encoding.decode_blob)](
                     ReadContext& ctx) -> Result<AnyIR> {
      const std::string text = input_as_text(ctx.input);
      ir::Blob blob;
      if (decode_blob) {
        CVT_TRY_ASSIGN(blob, decode_blob(text, ctx));
      } else {
        CVT_TRY_ASSIGN(blob.bytes, decode(text, ctx));
      }
      CVT_TRY(ctx.limits.check_decompressed(blob.bytes.size()));
      if (blob.filename.empty()) blob.filename = ctx.filename;
      return blob;
    };
  }
  registry.add_codec(std::move(codec));
}

void add_text_transform(Registry& registry, TextTransform transform) {
  registry.add_format(describe(transform.id, std::move(transform.name), std::move(transform.description),
                               std::move(transform.extensions), std::move(transform.mime), transform.popularity,
                               std::move(transform.sniff)));

  Codec codec;
  codec.format_id = transform.id;
  codec.kind = Kind::PlainText;
  codec.lossy = transform.lossy;
  codec.write_options = std::move(transform.encode_options);
  codec.read_options = std::move(transform.decode_options);

  if (transform.encode) {
    codec.write = [encode = std::move(transform.encode)](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(const std::string text, encode(value.get<ir::PlainText>().text, ctx));
      CVT_TRY(ctx.limits.check_output(text.size()));
      return to_bytes(text);
    };
  }
  if (transform.decode) {
    codec.read = [decode = std::move(transform.decode)](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY(ctx.limits.check_decompressed(static_cast<std::uint64_t>(ctx.input.size()) * 4));
      // Unlike binary encodings, transformed text may legitimately contain
      // non-ASCII characters in a legacy encoding: transcode, do not mangle.
      const util::EncodingGuess guess = util::guess_encoding(ctx.input);
      if (!guess.confident) ctx.warn("The text is not valid Unicode; it was read as Windows-1252.");
      ir::PlainText out;
      CVT_TRY_ASSIGN(out.text, decode(util::decode_text(ctx.input, guess.encoding).text, ctx));
      out.text = util::sanitize_utf8(out.text);
      out.source_encoding = "utf-8";
      return out;
    };
  }
  registry.add_codec(std::move(codec));
}

}  // namespace cvt::encoding

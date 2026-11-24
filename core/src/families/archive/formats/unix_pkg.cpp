// Convertible core - archive family: Debian (.deb) and RPM (.rpm) packages, read only.
//
// deb (deb(5)): an ar archive whose members are "debian-binary" ("2.0\n"),
// "control.tar[.gz|.xz|.zst]" and "data.tar[.gz|.xz|.zst|.bz2|.lzma]".
// The result is what `dpkg-deb -R` extracts: the data.tar entries, and the
// control files (control, md5sums, maintainer scripts...) under "DEBIAN/".
//
// rpm (RPM file format, rpm.org "RPM V4 package format"): a 96-byte lead
// (magic ED AB EE DB), a signature header padded to 8 bytes, the main
// header, then the payload: a newc cpio archive compressed as the main
// header's PAYLOADCOMPRESSOR tag says. Paths lose their leading "./".
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <format>

#include "families/archive/compression.hpp"
#include "families/archive/formats/tar.hpp"
#include "families/archive/formats/unix_internal.hpp"
#include "util/text.hpp"

namespace cvt::archive::unixfmt {

namespace {

// Decompress a package member as its name (or, failing that, its content) says.
Result<Bytes> unpack(std::string_view what, std::optional<compression::Method> method, ByteView data, EntrySink& sink,
                     const Context& ctx, double from, double to) {
  if (!method) method = compression::detect(data);
  if (!method || *method == compression::Method::Store) {
    CVT_TRY(sink.charge(0));
    return to_bytes(data);
  }
  if (!compression::can_decode(*method)) {
    return fail(Status::Decode, "{} is compressed with {}, which this build cannot decompress", what,
                compression::method_name(*method));
  }
  compression::DecodeOptions d;
  d.max_output = sink.budget();
  d.progress = ctx.progress.sub(from, to);
  return compression::decompress(*method, data, d);
}

std::optional<compression::Method> method_from_suffix(std::string_view name, std::string_view stem) {
  using compression::Method;
  if (name == stem) return Method::Store;
  const std::string_view suffix = name.substr(stem.size());
  if (suffix == ".gz") return Method::Gzip;
  if (suffix == ".xz") return Method::Xz;
  if (suffix == ".zst") return Method::Zstd;
  if (suffix == ".bz2") return Method::Bzip2;
  if (suffix == ".lzma") return Method::LzmaAlone;
  return std::nullopt;
}

}  // namespace

Result<ir::Archive> read_deb(ReadContext& ctx) {
  EntrySink sink(ctx, ctx.input.size());
  CVT_TRY_ASSIGN(const std::vector<ArMember> members, parse_ar(ctx.input, ctx));
  if (members.empty() || members[0].name != "debian-binary") return fail(Status::Decode, "not a Debian package");
  const std::string_view version = util::trim(as_chars(members[0].data));
  if (!version.starts_with("2.")) return fail(Status::Decode, "unsupported Debian package version '{}'", util::sanitize_utf8(version));
  const ArMember* control = nullptr;
  const ArMember* data = nullptr;
  for (const ArMember& m : members) {
    if (m.name.starts_with("control.tar") && control == nullptr) control = &m;
    if (m.name.starts_with("data.tar") && data == nullptr) data = &m;
  }
  if (data == nullptr) return fail(Status::Decode, "the Debian package has no data.tar member");
  if (control != nullptr) {
    CVT_TRY_ASSIGN(const Bytes tar, unpack("control.tar", method_from_suffix(control->name, "control.tar"), control->data,
                                           sink, ctx, 0.0, 0.1));
    CVT_TRY(read_tar(tar, sink, ctx, "DEBIAN/"));
  } else {
    ctx.warn("The Debian package has no control.tar member.");
  }
  CVT_TRY_ASSIGN(const Bytes tar, unpack("data.tar", method_from_suffix(data->name, "data.tar"), data->data, sink, ctx, 0.1, 0.8));
  CVT_TRY(read_tar(tar, sink, ctx));
  return sink.finish();
}

// ------------------------------------------------------------------------------- rpm

namespace {

constexpr std::uint32_t tag_payload_format = 1124;
constexpr std::uint32_t tag_payload_compressor = 1125;
constexpr std::uint32_t type_string = 6;

struct RpmHeader {
  std::size_t size = 0;  // bytes, including the 16-byte intro
  std::string payload_format;
  std::string payload_compressor;
};

// Parse a header structure at `at`: intro (magic 8E AD E8 01, 4 reserved,
// index count, data size), index entries (tag, type, offset, count), data.
Result<RpmHeader> parse_rpm_header(ByteView data, std::size_t at) {
  util::ByteReader r(data);
  if (!r.seek(at) || !r.consume("\x8E\xAD\xE8\x01")) return fail(Status::Decode, "RPM header is damaged");
  r.skip(4);
  const std::uint32_t count = r.u32be();
  const std::uint32_t store_size = r.u32be();
  if (!r.ok() || count > 100000 || store_size > (256u << 20)) return fail(Status::Decode, "RPM header is damaged");
  const std::uint64_t index_bytes = std::uint64_t{count} * 16;
  if (index_bytes + store_size > r.remaining()) return fail(Status::Decode, "RPM header is truncated");
  const ByteView index = r.bytes(static_cast<std::size_t>(index_bytes));
  const std::string_view store = as_chars(r.bytes(store_size));
  RpmHeader h;
  h.size = 16 + static_cast<std::size_t>(index_bytes) + store_size;
  util::ByteReader e(index);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t tag = e.u32be();
    const std::uint32_t type = e.u32be();
    const std::uint32_t offset = e.u32be();
    e.skip(4);
    if ((tag != tag_payload_format && tag != tag_payload_compressor) || type != type_string) continue;
    if (offset >= store.size()) return fail(Status::Decode, "RPM header entry is damaged");
    std::string_view value = store.substr(offset);
    value = value.substr(0, value.find('\0'));
    (tag == tag_payload_format ? h.payload_format : h.payload_compressor) = std::string(value);
  }
  return h;
}

}  // namespace

Result<ir::Archive> read_rpm(ReadContext& ctx) {
  const ByteView input = ctx.input;
  if (input.size() < 96 || as_chars(input.first(4)) != "\xED\xAB\xEE\xDB") return fail(Status::Decode, "not an RPM package");
  const auto major = std::to_integer<int>(input[4]);
  if (major < 3 || major > 4) return fail(Status::Decode, "unsupported RPM version {}", major);
  CVT_TRY_ASSIGN(const RpmHeader signature, parse_rpm_header(input, 96));
  const std::size_t main_at = 96 + (signature.size + 7) / 8 * 8;
  CVT_TRY_ASSIGN(const RpmHeader header, parse_rpm_header(input, main_at));
  const std::size_t payload_at = main_at + header.size;
  if (payload_at > input.size()) return fail(Status::Decode, "RPM package is truncated");
  if (!header.payload_format.empty() && header.payload_format != "cpio") {
    return fail(Status::Decode, "RPM payload format '{}' is not supported", util::sanitize_utf8(header.payload_format));
  }
  using compression::Method;
  std::optional<Method> method;
  const std::string& c = header.payload_compressor;
  if (c == "gzip") {
    method = Method::Gzip;
  } else if (c == "bzip2") {
    method = Method::Bzip2;
  } else if (c == "xz") {
    method = Method::Xz;
  } else if (c == "lzma") {
    method = Method::LzmaAlone;
  } else if (c == "zstd") {
    method = Method::Zstd;
  } else if (c == "identity") {
    method = Method::Store;
  } else if (c.empty()) {
    method = Method::Gzip;  // the historical default
  }
  const ByteView payload = input.subspan(payload_at);
  // Trust the content when it disagrees with the tag (repacked packages).
  if (is_cpio(payload)) {
    method = Method::Store;
  } else if (const auto detected = compression::detect(payload)) {
    method = detected;
  }
  EntrySink sink(ctx, input.size());
  CVT_TRY_ASSIGN(const Bytes cpio, unpack("the RPM payload", method, payload, sink, ctx, 0.0, 0.8));
  CVT_TRY(read_cpio(cpio, sink, ctx));
  return sink.finish();
}

}  // namespace cvt::archive::unixfmt

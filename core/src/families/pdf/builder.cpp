// Convertible core - pdf family: serialising a new PDF file.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "families/pdf/builder.hpp"

#include <algorithm>

#include "util/deflate.hpp"
#include "util/hash.hpp"

namespace cvt::pdf {

Bytes flate_encode(ByteView data, int level) {
  auto out = util::zlib_compress(data, std::clamp(level, 0, 9));
  if (!out) return Bytes(data.begin(), data.end());
  return std::move(*out);
}

Ref PdfBuilder::reserve() {
  const Ref r{next_++, 0};
  objects_[r.num];
  return r;
}

void PdfBuilder::set(Ref ref, Object object) {
  Entry& e = objects_[ref.num];
  e.set = true;
  e.stream = false;
  e.object = std::move(object);
  e.data.clear();
  if (ref.num >= next_) next_ = ref.num + 1;
}

void PdfBuilder::set_stream(Ref ref, Dict dict, ByteView data, bool compress) {
  Bytes encoded;
  bool flate = false;
  if (compress && data.size() > 32) {
    encoded = flate_encode(data, level_);
    flate = encoded.size() + 16 < data.size();
  }
  if (flate) {
    dict.set("Filter", Object::name("FlateDecode"));
  } else {
    encoded.assign(data.begin(), data.end());
  }
  set_stream_raw(ref, std::move(dict), std::move(encoded));
}

void PdfBuilder::set_stream_raw(Ref ref, Dict dict, Bytes data) {
  Entry& e = objects_[ref.num];
  dict.set("Length", static_cast<std::int64_t>(data.size()));
  e.set = true;
  e.stream = true;
  e.object = Object(std::move(dict));
  e.data = std::move(data);
  if (ref.num >= next_) next_ = ref.num + 1;
}

Result<Bytes> PdfBuilder::finish(Ref root, std::optional<Ref> info, std::string_view version, bool object_streams) {
  std::string out;
  out += "%PDF-";
  out += version;
  out += "\n%\xE2\xE3\xCF\xD3\n";
  const std::uint32_t size = next_ + 1 + static_cast<std::uint32_t>(objects_.size() / 100 + 1);
  struct XrefRow {
    int type = 0;
    std::uint64_t field2 = 0;
    std::uint32_t field3 = 0;
  };
  std::vector<XrefRow> xref(size);
  xref[0] = {0, 0, 65535};
  const auto write_indirect = [&](std::uint32_t num, const Object& dict, const Bytes* data) {
    xref[num] = {1, out.size(), 0};
    out += std::to_string(num);
    out += " 0 obj\n";
    serialize(dict, out);
    if (data != nullptr) {
      out += "\nstream\n";
      out.append(reinterpret_cast<const char*>(data->data()), data->size());
      out += "\nendstream";
    }
    out += "\nendobj\n";
  };
  std::vector<std::uint32_t> packed;
  for (const auto& [num, entry] : objects_) {
    if (!entry.set) return fail(Status::Internal, "PDF object {} was reserved but never written", num);
    if (entry.stream) {
      write_indirect(num, entry.object, &entry.data);
    } else if (object_streams) {
      packed.push_back(num);
    } else {
      write_indirect(num, entry.object, nullptr);
    }
  }
  std::uint32_t next = next_;
  for (std::size_t start = 0; start < packed.size(); start += 100) {
    const std::size_t end = std::min(packed.size(), start + 100);
    std::string header, body;
    const std::uint32_t stream_num = next++;
    for (std::size_t i = start; i < end; ++i) {
      const std::uint32_t num = packed[i];
      header += std::to_string(num) + " " + std::to_string(body.size()) + " ";
      serialize(objects_.at(num).object, body);
      body.push_back('\n');
      xref[num] = {2, stream_num, static_cast<std::uint32_t>(i - start)};
    }
    std::string content = header + "\n" + body;
    Dict dict;
    dict.set("Type", Object::name("ObjStm"));
    dict.set("N", static_cast<std::int64_t>(end - start));
    dict.set("First", static_cast<std::int64_t>(header.size() + 1));
    Bytes data = flate_encode(as_bytes(content), level_);
    dict.set("Filter", Object::name("FlateDecode"));
    dict.set("Length", static_cast<std::int64_t>(data.size()));
    write_indirect(stream_num, Object(std::move(dict)), &data);
  }
  // Cross-reference stream.
  const std::uint32_t xref_num = next++;
  xref.resize(std::max<std::size_t>(xref.size(), next));
  const std::size_t xref_offset = out.size();
  xref[xref_num] = {1, xref_offset, 0};
  const auto digest = util::md5(as_bytes(out));
  const std::string id(reinterpret_cast<const char*>(digest.data()), digest.size());
  Bytes rows;
  for (std::uint32_t i = 0; i < next; ++i) {
    const XrefRow& r = xref[i];
    rows.push_back(static_cast<std::byte>(r.type));
    for (int k = 3; k >= 0; --k) rows.push_back(static_cast<std::byte>((r.field2 >> (8 * k)) & 0xFF));
    rows.push_back(static_cast<std::byte>((r.field3 >> 8) & 0xFF));
    rows.push_back(static_cast<std::byte>(r.field3 & 0xFF));
  }
  if (out.size() > 0xFFFFFFFFull) return fail(Status::Encode, "the PDF is larger than 4 GiB");
  Dict xd;
  xd.set("Type", Object::name("XRef"));
  xd.set("Size", static_cast<std::int64_t>(next));
  xd.set("W", Array{Object(1), Object(4), Object(2)});
  xd.set("Root", root);
  if (info) xd.set("Info", *info);
  xd.set("ID", Array{Object::string(id, true), Object::string(id, true)});
  Bytes compressed = flate_encode(rows, level_);
  xd.set("Filter", Object::name("FlateDecode"));
  xd.set("Length", static_cast<std::int64_t>(compressed.size()));
  out += std::to_string(xref_num);
  out += " 0 obj\n";
  serialize(Object(std::move(xd)), out);
  out += "\nstream\n";
  out.append(reinterpret_cast<const char*>(compressed.data()), compressed.size());
  out += "\nendstream\nendobj\nstartxref\n";
  out += std::to_string(xref_offset);
  out += "\n%%EOF\n";
  return to_bytes(out);
}

}  // namespace cvt::pdf

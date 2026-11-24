// Convertible core - science family: tab-separated genomics formats
// (GFF3, GTF, BED, VCF, SAM) as tables, and BAM read through BGZF.
//
// Table model: sheet 0 holds the records with a header row of column names;
// header/comment/directive lines go to a one-column sheet named "header" so
// that writing the table back reproduces them.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <format>

#include "families/science/common.hpp"
#include "families/science/science.hpp"
#include "util/deflate.hpp"
#include "util/text.hpp"

namespace cvt::science {

namespace {

struct TabSpec {
  std::string id;
  std::vector<std::string> columns;        ///< fixed column names
  std::vector<std::string> comment_prefixes;
  std::string extra_prefix;                ///< name of unnamed extra columns ("TAG", "col")
  bool header_row_from_file = false;       ///< VCF: "#CHROM ..." line names the columns
};

const std::vector<std::string> gff_columns = {"seqid", "source", "type", "start", "end", "score", "strand",
                                              "phase", "attributes"};
const std::vector<std::string> bed_columns = {"chrom",      "chromStart", "chromEnd",   "name",
                                              "score",      "strand",     "thickStart", "thickEnd",
                                              "itemRgb",    "blockCount", "blockSizes", "blockStarts"};
const std::vector<std::string> sam_columns = {"QNAME", "FLAG", "RNAME", "POS",  "MAPQ", "CIGAR",
                                              "RNEXT", "PNEXT", "TLEN",  "SEQ", "QUAL"};
const std::vector<std::string> vcf_columns = {"CHROM", "POS", "ID", "REF", "ALT", "QUAL", "FILTER", "INFO"};

bool is_comment(std::string_view line, const TabSpec& spec) {
  for (const auto& prefix : spec.comment_prefixes) {
    if (line.starts_with(prefix)) return true;
  }
  return false;
}

/// Split GFF3 ("k=v;k=v") or GTF ("k \"v\"; k \"v\";") attributes.
std::vector<std::pair<std::string, std::string>> split_attributes(std::string_view text, bool gtf) {
  std::vector<std::pair<std::string, std::string>> out;
  for (std::string_view part : util::split(text, ';')) {
    part = util::trim(part);
    if (part.empty()) continue;
    if (gtf) {
      const std::size_t space = part.find(' ');
      std::string_view value = space == std::string_view::npos ? std::string_view{} : util::trim(part.substr(space));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
      out.emplace_back(std::string(part.substr(0, space)), std::string(value));
    } else {
      const std::size_t eq = part.find('=');
      if (eq == std::string_view::npos) {
        out.emplace_back(std::string(part), "");
      } else {
        out.emplace_back(std::string(part.substr(0, eq)), std::string(part.substr(eq + 1)));
      }
    }
  }
  return out;
}

Result<AnyIR> read_tab(const TabSpec& spec, ReadContext& ctx, ByteView input) {
  const std::string text = input_text(input);
  const auto lines = lines_of(text);
  std::vector<std::string> header_lines;
  std::vector<std::string> fasta_lines;
  std::vector<std::string> columns = spec.columns;
  std::vector<std::vector<ir::Cell>> rows;
  bool in_fasta = false;
  std::uint64_t cells = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
    const std::string_view line = lines[i];
    if (in_fasta) {
      fasta_lines.emplace_back(line);
      continue;
    }
    if (line.empty()) continue;
    if (spec.id == "gff3" && line.starts_with("##FASTA")) {
      in_fasta = true;
      continue;
    }
    if (spec.header_row_from_file && line.starts_with("#CHROM")) {
      columns.clear();
      for (const auto field : util::split(line.substr(1), '\t')) columns.emplace_back(field);
      continue;
    }
    if (is_comment(line, spec)) {
      header_lines.emplace_back(line);
      continue;
    }
    const auto fields = util::split(line, '\t');
    if (fields.size() < 3) return fail(Status::Decode, "line {} is not a tab-separated {} record", i + 1, spec.id);
    std::vector<ir::Cell> row;
    row.reserve(fields.size());
    for (const auto field : fields) row.push_back(auto_cell(field));
    cells += row.size();
    CVT_TRY(check_cells(ctx, cells));
    rows.push_back(std::move(row));
  }
  if (rows.empty() && header_lines.empty()) return fail(Status::Decode, "no {} records found", spec.id);
  std::size_t width = columns.size();
  for (const auto& row : rows) width = std::max(width, row.size());
  for (std::size_t c = columns.size(); c < width; ++c) {
    columns.push_back(spec.extra_prefix + std::to_string(c - spec.columns.size() + 1));
  }
  ir::Sheet sheet = header_sheet(spec.id, columns);
  for (auto& row : rows) sheet.rows.push_back(std::move(row));
  // Optional: expand attributes of GFF3/GTF into their own columns.
  if ((spec.id == "gff3" || spec.id == "gtf") && ctx.options.get_bool("expand_attributes", false)) {
    std::vector<std::string> keys;
    std::vector<std::vector<std::pair<std::string, std::string>>> parsed;
    for (std::size_t r = 1; r < sheet.rows.size(); ++r) {
      parsed.push_back(split_attributes(cell_text(sheet, r, 8), spec.id == "gtf"));
      for (const auto& [key, value] : parsed.back()) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
      }
    }
    for (const auto& key : keys) sheet.rows[0].push_back(ir::Cell::string("attr:" + key));
    for (std::size_t r = 1; r < sheet.rows.size(); ++r) {
      auto& row = sheet.rows[r];
      row.resize(9 + keys.size());
      for (const auto& [key, value] : parsed[r - 1]) {
        const auto at = std::find(keys.begin(), keys.end(), key) - keys.begin();
        row[9 + static_cast<std::size_t>(at)] = auto_cell(value);
      }
    }
  }
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  if (!header_lines.empty()) table.sheets.push_back(lines_sheet("header", header_lines));
  if (!fasta_lines.empty()) table.sheets.push_back(lines_sheet("fasta", fasta_lines));
  return table;
}

Result<Bytes> write_tab(const TabSpec& spec, const AnyIR& value, WriteContext& ctx) {
  const ir::Table& table = value.get<ir::Table>();
  if (table.sheets.empty()) return fail(Status::Encode, "the table has no sheets");
  const ir::Sheet* data = &table.sheets.front();
  for (const auto& sheet : table.sheets) {
    if (!util::iequals(sheet.name, "header") && !util::iequals(sheet.name, "fasta")) {
      data = &sheet;
      break;
    }
  }
  std::string out;
  if (const ir::Sheet* header = find_sheet(table, "header"); header != nullptr && header != data) {
    for (const auto& line : sheet_lines(*header)) out += line + '\n';
  } else if (spec.id == "gff3") {
    out += "##gff-version 3\n";
  } else if (spec.id == "vcf-variant") {
    out += "##fileformat=VCFv4.2\n";
  }
  // Does row 0 name the columns? (It does for every table this family reads,
  // and for CSV files whose first row matches the format's columns.)
  bool skip_first = data->has_header;
  if (!data->rows.empty() && !spec.columns.empty()) {
    skip_first = skip_first || util::iequals(util::trim(data->rows[0].empty() ? "" : data->rows[0][0].text),
                                             spec.columns[0]) ||
                 (spec.id == "vcf-variant" && util::iequals(data->rows[0].empty() ? "" : data->rows[0][0].text, "#CHROM"));
  }
  // GFF/GTF tables with expanded attributes: write only the nine real columns.
  std::size_t max_columns = SIZE_MAX;
  if ((spec.id == "gff3" || spec.id == "gtf") && column_of(*data, "attributes") == 8) max_columns = 9;
  if (spec.id == "vcf-variant") {
    std::string titles;
    if (skip_first && !data->rows.empty()) {
      for (const auto& c : data->rows[0]) titles += (titles.empty() ? "#" : "\t") + c.text;
    } else {
      for (const auto& c : vcf_columns) titles += (titles.empty() ? "#" : "\t") + c;
    }
    out += titles + '\n';
  }
  for (std::size_t r = skip_first ? 1 : 0; r < data->rows.size(); ++r) {
    if ((r & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(r) / static_cast<double>(data->rows.size())));
    const auto& row = data->rows[r];
    std::size_t n = std::min(row.size(), max_columns);
    while (n > spec.columns.size() && row[n - 1].text.empty()) --n;  // trailing empty extras
    std::string line;
    for (std::size_t c = 0; c < n; ++c) {
      if (c) line += '\t';
      std::string text = row[c].text;
      if (text.empty() && (spec.id == "gff3" || spec.id == "gtf" || spec.id == "vcf-variant")) text = ".";
      if (text.find_first_of("\t\n\r") != std::string::npos) {
        return fail(Status::Encode, "cell ({}, {}) contains a tab or line break", r, c);
      }
      line += text;
    }
    out += line + '\n';
  }
  if (const ir::Sheet* fasta = find_sheet(table, "fasta"); fasta != nullptr && spec.id == "gff3") {
    out += "##FASTA\n";
    for (const auto& line : sheet_lines(*fasta)) out += line + '\n';
  }
  CVT_TRY(ctx.limits.check_output(out.size()));
  return to_bytes(out);
}

int sniff_tab(ByteView data, std::string_view name, std::string_view id) {
  const std::string_view head = head_text(data, 4096);
  if (id == "gff3") {
    if (head.starts_with("##gff-version 3")) return 95;
    if (head.starts_with("##gff-version")) return 60;
    return 0;
  }
  if (id == "vcf-variant") return head.starts_with("##fileformat=VCF") ? 97 : 0;
  if (id == "sam") {
    if (head.starts_with("@HD\tVN:") || head.starts_with("@SQ\tSN:")) return 95;
    return 0;
  }
  auto lines = lines_of(head);
  if (data.size() > head.size() && !lines.empty()) lines.pop_back();  // possibly cut short
  std::size_t checked = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    if (line.empty() || line.starts_with('#') || line.starts_with("track") || line.starts_with("browser")) continue;
    const auto fields = util::split(line, '\t');
    if (id == "gtf") {
      if (fields.size() != 9 || !to_int(fields[3]) || !to_int(fields[4])) return 0;
      if (fields[8].find(" \"") == std::string_view::npos) return 0;
    } else {  // bed
      if (fields.size() < 3 || fields.size() > 12 || !to_int(fields[1]) || !to_int(fields[2])) return 0;
    }
    if (++checked >= 3) break;
  }
  if (checked == 0) return 0;
  if (id == "gtf") return has_extension(name, "gtf") ? 90 : 75;
  return has_extension(name, "bed") ? 85 : 35;
}

void register_tab(Registry& registry, TabSpec spec, FormatSpec format, std::vector<OptionSpec> read_options = {}) {
  const std::string id = spec.id;
  format.id = id;
  format.sniff = [id](ByteView data, std::string_view name) { return sniff_tab(data, name, id); };
  add_format(registry, std::move(format));
  add_codec(
      registry, id, Kind::Table,
      [spec](ReadContext& ctx) -> Result<AnyIR> { return read_tab(spec, ctx, ctx.input); },
      [spec](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> { return write_tab(spec, value, ctx); },
      std::move(read_options));
}

// --------------------------------------------------------------------------- BAM

std::string bam_cigar(util::ByteReader& reader, std::uint32_t count) {
  static constexpr std::string_view ops = "MIDNSHP=X";
  std::string out;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t v = reader.u32le();
    const std::uint32_t op = v & 15;
    out += std::to_string(v >> 4);
    out += op < ops.size() ? ops[op] : '?';
  }
  return out.empty() ? "*" : out;
}

Result<std::string> bam_tag_value(util::ByteReader& reader, char type) {
  switch (type) {
    case 'A': return std::string(1, static_cast<char>(reader.u8()));
    case 'c': return std::to_string(reader.i8());
    case 'C': return std::to_string(reader.u8());
    case 's': return std::to_string(reader.i16le());
    case 'S': return std::to_string(reader.u16le());
    case 'i': return std::to_string(reader.i32le());
    case 'I': return std::to_string(reader.u32le());
    case 'f': return fmt_num(static_cast<double>(reader.f32le()), 9);
    case 'Z':
    case 'H': return std::string(reader.cstring());
    default: return fail(Status::Decode, "unknown BAM tag type '{}'", type);
  }
}

Result<AnyIR> read_bam(ReadContext& ctx) {
  if (ctx.input.size() < 18) return fail(Status::Decode, "BAM file is truncated");
  CVT_TRY_ASSIGN(const Bytes raw, util::gzip_decompress(ctx.input, ctx.limits.max_decompressed_bytes));
  util::ByteReader reader(raw);
  if (!reader.consume("BAM\x01")) return fail(Status::Decode, "missing BAM magic");
  const std::uint32_t l_text = reader.u32le();
  const std::string_view header_text = reader.chars(l_text);
  const std::uint32_t n_ref = reader.u32le();
  if (!reader.ok() || n_ref > reader.remaining() / 8) return fail(Status::Decode, "BAM header is truncated");
  std::vector<std::string> refs;
  for (std::uint32_t i = 0; i < n_ref; ++i) {
    const std::uint32_t l_name = reader.u32le();
    std::string_view name = reader.chars(l_name);
    if (!name.empty() && name.back() == '\0') name.remove_suffix(1);
    refs.emplace_back(name);
    (void)reader.u32le();
  }
  if (!reader.ok()) return fail(Status::Decode, "BAM reference list is truncated");
  const auto ref_name = [&refs](std::int32_t id) -> std::string {
    if (id < 0 || static_cast<std::size_t>(id) >= refs.size()) return "*";
    return refs[static_cast<std::size_t>(id)];
  };
  std::vector<std::string> header_lines;
  // Named: lines_of returns views, which a temporary string would leave dangling.
  const std::string header_string(header_text.substr(0, header_text.find('\0')));
  for (const auto line : lines_of(header_string)) {
    if (!line.empty()) header_lines.emplace_back(line);
  }
  std::vector<std::vector<ir::Cell>> rows;
  std::size_t width = sam_columns.size();
  std::uint64_t cells = 0;
  while (reader.remaining() >= 4) {
    if ((rows.size() & 1023) == 0) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(reader.position()) / static_cast<double>(reader.size())));
    }
    const std::uint32_t block_size = reader.u32le();
    if (block_size < 32 || block_size > reader.remaining()) return fail(Status::Decode, "BAM record is truncated");
    util::ByteReader rec = reader.sub(block_size);
    const std::int32_t ref_id = rec.i32le();
    const std::int32_t pos = rec.i32le();
    const std::uint8_t l_read_name = rec.u8();
    const std::uint8_t mapq = rec.u8();
    (void)rec.u16le();  // bin
    const std::uint16_t n_cigar = rec.u16le();
    const std::uint16_t flag = rec.u16le();
    const std::int32_t l_seq = rec.i32le();
    const std::int32_t next_ref = rec.i32le();
    const std::int32_t next_pos = rec.i32le();
    const std::int32_t tlen = rec.i32le();
    if (l_seq < 0 || static_cast<std::uint64_t>(l_seq) > rec.remaining()) return fail(Status::Decode, "bad BAM sequence length");
    std::string_view name = rec.chars(l_read_name);
    if (!name.empty() && name.back() == '\0') name.remove_suffix(1);
    const std::string cigar = bam_cigar(rec, n_cigar);
    static constexpr std::string_view bases = "=ACMGRSVTWYHKDBN";
    const ByteView packed = rec.bytes((static_cast<std::size_t>(l_seq) + 1) / 2);
    std::string seq;
    for (std::int32_t i = 0; i < l_seq; ++i) {
      const auto byte = static_cast<std::uint8_t>(packed[static_cast<std::size_t>(i) / 2]);
      seq.push_back(bases[(i & 1) ? (byte & 15) : (byte >> 4)]);
    }
    const ByteView qual_bytes = rec.bytes(static_cast<std::size_t>(l_seq));
    std::string qual;
    if (!qual_bytes.empty() && static_cast<std::uint8_t>(qual_bytes[0]) == 0xFF) {
      qual = "*";
    } else {
      for (const std::byte b : qual_bytes) qual.push_back(static_cast<char>(std::min(93, static_cast<int>(b)) + 33));
    }
    if (!rec.ok()) return fail(Status::Decode, "BAM record is truncated");
    std::vector<ir::Cell> row = {
        text_cell(name),
        int_cell(flag),
        text_cell(ref_name(ref_id)),
        int_cell(static_cast<std::int64_t>(pos) + 1),
        int_cell(mapq),
        text_cell(cigar),
        text_cell(next_ref == ref_id && ref_id >= 0 ? std::string("=") : ref_name(next_ref)),
        int_cell(static_cast<std::int64_t>(next_pos) + 1),
        int_cell(tlen),
        text_cell(seq.empty() ? std::string("*") : seq),
        text_cell(qual.empty() ? std::string("*") : qual),
    };
    while (rec.remaining() >= 3) {
      const std::string_view tag = rec.chars(2);
      const char type = static_cast<char>(rec.u8());
      std::string field = std::string(tag) + ":";
      if (type == 'B') {
        const char sub = static_cast<char>(rec.u8());
        const std::uint32_t count = rec.u32le();
        if (count > rec.remaining()) return fail(Status::Decode, "BAM array tag is truncated");
        field += std::string("B:") + sub;
        for (std::uint32_t k = 0; k < count && rec.ok(); ++k) {
          CVT_TRY_ASSIGN(const std::string v, bam_tag_value(rec, sub));
          field += "," + v;
        }
      } else {
        CVT_TRY_ASSIGN(const std::string v, bam_tag_value(rec, type));
        const char sam_type = (type == 'c' || type == 'C' || type == 's' || type == 'S' || type == 'I') ? 'i' : type;
        field += std::string(1, sam_type) + ":" + v;
      }
      if (!rec.ok()) return fail(Status::Decode, "BAM tag is truncated");
      row.push_back(text_cell(field));
    }
    width = std::max(width, row.size());
    cells += row.size();
    CVT_TRY(check_cells(ctx, cells));
    rows.push_back(std::move(row));
  }
  std::vector<std::string> columns = sam_columns;
  for (std::size_t c = columns.size(); c < width; ++c) columns.push_back("TAG" + std::to_string(c - sam_columns.size() + 1));
  ir::Sheet sheet = header_sheet("sam", columns);
  for (auto& row : rows) sheet.rows.push_back(std::move(row));
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  if (!header_lines.empty()) table.sheets.push_back(lines_sheet("header", header_lines));
  return table;
}

}  // namespace

void register_feature_tables(Registry& registry) {
  const std::vector<OptionSpec> gff_options = {OptionSpec::boolean(
      "expand_attributes", "Expand attributes", false,
      "Add one column per attribute key (attr:ID, attr:Name, ...) next to the raw attributes column.")};
  register_tab(registry, {"gff3", gff_columns, {"#"}, "col", false},
               {.name = "GFF3 annotation",
                .description = "Generic Feature Format version 3 genome annotation.",
                .extensions = {"gff3", "gff"},
                .mime = "text/x-gff3",
                .popularity = 25},
               gff_options);
  register_tab(registry, {"gtf", gff_columns, {"#"}, "col", false},
               {.name = "GTF annotation",
                .description = "Gene Transfer Format (GFF2 dialect) genome annotation.",
                .extensions = {"gtf"},
                .mime = "text/x-gtf",
                .popularity = 20},
               gff_options);
  register_tab(registry, {"bed", bed_columns, {"#", "track", "browser"}, "col", false},
               {.name = "BED intervals",
                .description = "Browser Extensible Data genomic intervals.",
                .extensions = {"bed"},
                .mime = "text/x-bed",
                .popularity = 25});
  register_tab(registry, {"vcf-variant", vcf_columns, {"##"}, "col", true},
               {.name = "VCF variants",
                .description = "Variant Call Format: genomic variants with per-sample genotypes.",
                .extensions = {"vcf"},
                .mime = "text/x-vcf",
                .popularity = 30});
  register_tab(registry, {"sam", sam_columns, {"@"}, "TAG", false},
               {.name = "SAM alignments",
                .description = "Sequence Alignment/Map text format.",
                .extensions = {"sam"},
                .mime = "text/x-sam",
                .popularity = 25});

  add_format(registry, {.id = "bam",
                        .name = "BAM alignments",
                        .description = "Binary (BGZF-compressed) Sequence Alignment/Map.",
                        .extensions = {"bam"},
                        .mime = "application/x-bam",
                        .popularity = 25,
                        .signatures = {Signature::at(0, "1F 8B 08 04 ?? ?? ?? ?? ?? ?? 06 00 'BC' 02 00")}});
  add_codec(registry, "bam", Kind::Table, read_bam, nullptr);
}

}  // namespace cvt::science

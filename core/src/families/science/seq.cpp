// Convertible core - science family: sequence files (FASTA, FASTQ, GenBank,
// EMBL, UniProt/Swiss-Prot text) on the shared sequence record model.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include <algorithm>
#include <cmath>
#include <format>

#include "engine/standard_options.hpp"
#include "families/science/bio.hpp"
#include "families/science/science.hpp"
#include "util/text.hpp"

namespace cvt::science::bio {

// --------------------------------------------------------------------------- model

ir::Table records_to_table(const std::vector<SeqRecord>& records) {
  bool any_quality = false;
  for (const auto& r : records) any_quality = any_quality || !r.quality.empty();
  std::vector<std::string> header = {"id", "description", "sequence"};
  if (any_quality) header.emplace_back("quality");
  ir::Sheet sheet = header_sheet("sequences", header);
  for (const auto& r : records) {
    std::vector<ir::Cell> row = {text_cell(r.id), text_cell(r.description), text_cell(r.sequence)};
    if (any_quality) row.push_back(text_cell(r.quality));
    sheet.rows.push_back(std::move(row));
  }
  ir::Table table;
  table.sheets.push_back(std::move(sheet));
  return table;
}

Result<std::vector<SeqRecord>> table_to_records(const ir::Table& table) {
  if (table.sheets.empty()) return fail(Status::Encode, "the table has no sheets");
  const ir::Sheet* sheet = find_sheet(table, "sequences");
  if (sheet == nullptr) sheet = &table.sheets.front();
  std::vector<SeqRecord> records;
  int id = column_of(*sheet, "id");
  int description = column_of(*sheet, "description");
  int sequence = column_of(*sheet, "sequence");
  int quality = column_of(*sheet, "quality");
  std::size_t first = 1;
  if (sequence < 0) {
    // No header: the first column is the id, the last one the sequence.
    first = 0;
    const std::size_t columns = sheet->column_count();
    if (columns == 0) return records;
    id = 0;
    sequence = static_cast<int>(columns - 1);
    description = columns >= 3 ? 1 : -1;
    quality = -1;
    if (columns == 1) id = -1;
  }
  for (std::size_t row = first; row < sheet->rows.size(); ++row) {
    SeqRecord record;
    record.id = std::string(cell_text(*sheet, row, id));
    record.description = std::string(cell_text(*sheet, row, description));
    record.sequence = std::string(cell_text(*sheet, row, sequence));
    record.quality = std::string(cell_text(*sheet, row, quality));
    if (record.id.empty() && record.sequence.empty()) continue;
    if (record.id.empty()) record.id = "seq" + std::to_string(records.size() + 1);
    std::erase_if(record.sequence, [](char c) { return util::is_ascii_space(c); });
    records.push_back(std::move(record));
  }
  return records;
}

Json records_to_json(const std::vector<SeqRecord>& records) {
  Json list = Json::array();
  for (const auto& r : records) {
    Json item = Json::object();
    item["id"] = r.id;
    if (!r.name.empty()) item["name"] = r.name;
    item["description"] = r.description;
    if (!r.molecule_type.empty()) item["molecule_type"] = r.molecule_type;
    if (!r.topology.empty()) item["topology"] = r.topology;
    item["length"] = static_cast<std::uint64_t>(r.sequence.size());
    item["sequence"] = r.sequence;
    if (!r.quality.empty()) {
      Json scores = Json::array();
      for (const char c : r.quality) scores.push_back(static_cast<int>(static_cast<unsigned char>(c)) - 33);
      item["phred_quality"] = std::move(scores);
    }
    if (!r.annotations.empty()) item["annotations"] = r.annotations;
    if (!r.features.empty()) {
      Json features = Json::array();
      for (const auto& f : r.features) {
        Json feature = Json::object();
        feature["type"] = f.type;
        feature["location"] = f.location;
        Json qualifiers = Json::object();
        for (const auto& [key, value] : f.qualifiers) {
          if (Json* existing = qualifiers.find(key)) {
            if (!existing->is_array()) {
              Json list_value = Json::array();
              list_value.push_back(*existing);
              *existing = std::move(list_value);
            }
            existing->push_back(value);
          } else {
            qualifiers[key] = value;
          }
        }
        feature["qualifiers"] = std::move(qualifiers);
        features.push_back(std::move(feature));
      }
      item["features"] = std::move(features);
    }
    list.push_back(std::move(item));
  }
  return Json::object({{"records", std::move(list)}});
}

Result<std::vector<SeqRecord>> json_to_records(const Json& value) {
  const Json* list = &value;
  if (value.is_object()) {
    list = value.find("records");
    if (list == nullptr) return fail(Status::Encode, "the data has no \"records\" list of sequences");
  }
  if (!list->is_array()) return fail(Status::Encode, "the sequence records must be a list");
  std::vector<SeqRecord> records;
  for (const Json& item : list->elements()) {
    if (!item.is_object()) continue;
    SeqRecord r;
    r.id = item["id"].as_string();
    r.name = item["name"].as_string();
    r.description = item["description"].as_string();
    r.molecule_type = item["molecule_type"].as_string();
    r.topology = item["topology"].as_string();
    r.sequence = item["sequence"].as_string();
    if (const Json* scores = item.find("phred_quality"); scores != nullptr && scores->is_array()) {
      for (const Json& q : scores->elements()) {
        r.quality.push_back(static_cast<char>(std::clamp<std::int64_t>(q.as_int64(), 0, 93) + 33));
      }
    } else {
      r.quality = item["quality"].as_string();
    }
    if (const Json* a = item.find("annotations"); a != nullptr && a->is_object()) r.annotations = *a;
    if (const Json* features = item.find("features"); features != nullptr && features->is_array()) {
      for (const Json& f : features->elements()) {
        Feature feature;
        feature.type = f["type"].as_string();
        feature.location = f["location"].as_string();
        for (const auto& [key, v] : f["qualifiers"].items()) {
          if (v.is_array()) {
            for (const Json& each : v.elements()) {
              feature.qualifiers.emplace_back(key, each.is_string() ? each.as_string() : util::dump_json(each));
            }
          } else {
            feature.qualifiers.emplace_back(key, v.is_string() ? v.as_string() : util::dump_json(v));
          }
        }
        r.features.push_back(std::move(feature));
      }
    }
    records.push_back(std::move(r));
  }
  return records;
}

void add_record_codecs(Registry& registry, RecordCodec codec) {
  const auto reader = [read = codec.read](ReadContext& ctx) -> Result<std::vector<SeqRecord>> {
    const std::string text = input_text(ctx.input);
    return read(text, ctx);
  };
  ReadFn read_table;
  ReadFn read_data;
  WriteFn write_table;
  WriteFn write_data;
  if (codec.read) {
    read_table = [reader](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(auto records, reader(ctx));
      return records_to_table(records);
    };
    read_data = [reader](ReadContext& ctx) -> Result<AnyIR> {
      CVT_TRY_ASSIGN(auto records, reader(ctx));
      return ir::Data{records_to_json(records)};
    };
  }
  if (codec.write) {
    write_table = [write = codec.write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(auto records, table_to_records(value.get<ir::Table>()));
      CVT_TRY_ASSIGN(std::string text, write(records, ctx));
      return to_bytes(text);
    };
    write_data = [write = codec.write](const AnyIR& value, WriteContext& ctx) -> Result<Bytes> {
      CVT_TRY_ASSIGN(auto records, json_to_records(value.get<ir::Data>().value));
      CVT_TRY_ASSIGN(std::string text, write(records, ctx));
      return to_bytes(text);
    };
  }
  // Where the Table view drops annotations, the Data route is the cheaper
  // (preferred) one so that same-format and flat-file conversions keep them.
  const int table_cost = codec.lossy_table ? 12 : 10;
  const int data_cost = codec.lossy_table ? 10 : 12;
  add_codec(registry, codec.id, Kind::Table, read_table, write_table, codec.read_options, codec.write_options,
            table_cost, table_cost, codec.lossy_table);
  add_codec(registry, codec.id, Kind::Data, read_data, write_data, codec.read_options, codec.write_options, data_cost,
            data_cost);
}

namespace {

std::string upper_letters(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (!util::is_ascii_space(c) && !util::is_ascii_digit(c) && c != '/') out.push_back(util::ascii_upper(c));
  }
  return out;
}

std::string wrap(std::string_view sequence, std::size_t width) {
  std::string out;
  if (width == 0) width = sequence.size() + 1;
  for (std::size_t i = 0; i < sequence.size(); i += width) {
    out.append(sequence.substr(i, width));
    out.push_back('\n');
  }
  return out;
}

void split_title(std::string_view title, SeqRecord& record) {
  title = util::trim(title);
  const std::size_t space = title.find_first_of(" \t");
  record.id = std::string(title.substr(0, space));
  record.name = record.id;
  record.description = space == std::string_view::npos ? std::string() : std::string(util::trim(title.substr(space)));
}

std::string full_title(const SeqRecord& r) {
  if (r.description.empty() || r.description == r.id) return r.id;
  if (r.description.starts_with(r.id + " ")) return r.description;
  return r.id + " " + r.description;
}

// --------------------------------------------------------------------------- FASTA

Result<std::vector<SeqRecord>> read_fasta(std::string_view text, ReadContext& ctx) {
  std::vector<SeqRecord> records;
  bool started = false;
  const auto lines = lines_of(text);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if ((i & 4095) == 0) CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
    const std::string_view line = lines[i];
    if (line.starts_with('>')) {
      SeqRecord record;
      split_title(line.substr(1), record);
      records.push_back(std::move(record));
      started = true;
      continue;
    }
    if (line.starts_with(';')) continue;  // old-style comment
    const std::string_view trimmed = util::trim(line);
    if (trimmed.empty()) continue;
    if (!started) return fail(Status::Decode, "FASTA data must start with a '>' title line");
    for (const char c : trimmed) {
      if (util::is_ascii_space(c)) continue;
      if (!(util::is_ascii_alpha(c) || c == '-' || c == '*' || c == '.' || c == '?')) {
        return fail(Status::Decode, "unexpected character '{}' in FASTA sequence on line {}", c, i + 1);
      }
      records.back().sequence.push_back(c);
    }
  }
  if (records.empty()) return fail(Status::Decode, "no FASTA records found");
  CVT_TRY(check_cells(ctx, records.size() * 3));
  return records;
}

Result<std::string> write_fasta(const std::vector<SeqRecord>& records, WriteContext& ctx) {
  const auto width = static_cast<std::size_t>(ctx.options.get_int("line_width", 60));
  std::string out;
  for (const auto& r : records) {
    out += '>' + full_title(r) + '\n';
    out += wrap(r.sequence, width);
  }
  return out;
}

int sniff_fasta(ByteView data, std::string_view name) {
  const std::string_view head = head_text(data, 2048);
  if (!head.starts_with('>')) return 0;
  const auto lines = lines_of(head);
  if (lines.size() < 2) return 30;
  for (std::size_t i = 1; i < lines.size() && i < 8; ++i) {
    const std::string_view line = util::trim(lines[i]);
    if (line.starts_with('>')) continue;
    for (const char c : line) {
      if (!(util::is_ascii_alpha(c) || c == '-' || c == '*' || c == '.')) return 0;
    }
  }
  return has_extension(name, "fasta") || has_extension(name, "fa") || has_extension(name, "fna") ||
                 has_extension(name, "faa")
             ? 92
             : 70;
}

// --------------------------------------------------------------------------- FASTQ

enum class QualityEncoding { Phred33, Phred64, Solexa };

int solexa_to_phred(int solexa) {
  return static_cast<int>(std::lround(10.0 * std::log10(std::pow(10.0, solexa / 10.0) + 1.0)));
}

int phred_to_solexa(int phred) {
  if (phred <= 0) return -5;
  const double value = 10.0 * std::log10(std::pow(10.0, phred / 10.0) - 1.0);
  return std::max(-5, static_cast<int>(std::lround(value)));
}

Result<std::vector<SeqRecord>> read_fastq(std::string_view text, ReadContext& ctx) {
  std::vector<SeqRecord> records;
  const auto lines = lines_of(text);
  std::size_t i = 0;
  int low = 255;
  int high = 0;
  while (i < lines.size()) {
    if ((records.size() & 1023) == 0) {
      CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
    }
    if (util::trim(lines[i]).empty()) {
      ++i;
      continue;
    }
    if (!lines[i].starts_with('@')) return fail(Status::Decode, "FASTQ record must start with '@' (line {})", i + 1);
    SeqRecord record;
    split_title(lines[i].substr(1), record);
    ++i;
    // The sequence may span several lines up to the '+' separator.
    while (i < lines.size() && !lines[i].starts_with('+')) record.sequence += util::trim(lines[i++]);
    if (i >= lines.size()) return fail(Status::Decode, "FASTQ record '{}' is truncated", record.id);
    ++i;  // '+' line
    while (i < lines.size() && record.quality.size() < record.sequence.size()) record.quality += lines[i++];
    if (record.quality.size() != record.sequence.size()) {
      return fail(Status::Decode, "FASTQ record '{}' has {} quality scores for {} bases", record.id,
                  record.quality.size(), record.sequence.size());
    }
    for (const char c : record.quality) {
      const int v = static_cast<unsigned char>(c);
      if (v < 33 || v > 126) return fail(Status::Decode, "invalid FASTQ quality character in '{}'", record.id);
      low = std::min(low, v);
      high = std::max(high, v);
    }
    records.push_back(std::move(record));
  }
  if (records.empty()) return fail(Status::Decode, "no FASTQ records found");
  CVT_TRY(check_cells(ctx, records.size() * 4));
  std::string encoding = ctx.options.get_string("quality_encoding_in", "auto");
  if (encoding == "auto") {
    if (low < 59) {
      encoding = "phred33";
    } else if (low < 64) {
      encoding = "solexa";
    } else if (high > 74) {
      encoding = "phred64";
    } else {
      encoding = "phred33";
    }
    if (encoding != "phred33") ctx.warn(std::format("FASTQ qualities detected as {}; converted to Phred+33.", encoding));
  }
  for (auto& r : records) {
    for (char& c : r.quality) {
      int q = static_cast<unsigned char>(c);
      if (encoding == "phred64") {
        q = std::max(0, q - 64);
      } else if (encoding == "solexa") {
        q = std::max(0, solexa_to_phred(q - 64));
      } else {
        q -= 33;
      }
      c = static_cast<char>(std::min(q, 93) + 33);
    }
  }
  return records;
}

Result<std::string> write_fastq(const std::vector<SeqRecord>& records, WriteContext& ctx) {
  const std::string encoding = ctx.options.get_string("quality_encoding", "phred33");
  const int default_quality = static_cast<int>(ctx.options.get_int("default_quality", 40));
  std::string out;
  bool filled = false;
  for (const auto& r : records) {
    out += '@' + full_title(r) + '\n' + r.sequence + "\n+\n";
    std::string quality = r.quality;
    if (quality.size() != r.sequence.size()) {
      quality.assign(r.sequence.size(), static_cast<char>(default_quality + 33));
      filled = true;
    }
    for (char& c : quality) {
      const int q = static_cast<unsigned char>(c) - 33;
      int v = q + 33;
      if (encoding == "phred64") v = std::min(q, 62) + 64;
      if (encoding == "solexa") v = std::min(phred_to_solexa(q), 62) + 64;
      c = static_cast<char>(v);
    }
    out += quality + '\n';
  }
  if (filled) ctx.warn(std::format("Records without qualities were given quality {}.", default_quality));
  return out;
}

int sniff_fastq(ByteView data, std::string_view name) {
  const auto lines = lines_of(head_text(data, 4096));
  if (lines.size() < 4 || !lines[0].starts_with('@') || !lines[2].starts_with('+')) return 0;
  if (lines[1].size() != lines[3].size()) return 0;
  return has_extension(name, "fastq") || has_extension(name, "fq") ? 95 : 85;
}

// --------------------------------------------------------------------------- GenBank / EMBL shared

std::string strip_quotes(std::string_view value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
  return util::replace_all(value, "\"\"", "\"");
}

/// Parse feature-table lines (key at column `key_col`, location/qualifiers at
/// column `value_col`, after removing the line prefix).
void parse_features(const std::vector<std::string_view>& lines, std::vector<Feature>& out) {
  Feature* current = nullptr;
  bool in_qualifier = false;
  bool open_quote = false;
  for (const std::string_view raw : lines) {
    if (raw.size() < 6) continue;
    const std::string_view key = util::trim(raw.substr(0, std::min<std::size_t>(raw.size(), 16)));
    const std::string_view value = raw.size() > 16 ? util::trim(raw.substr(16)) : std::string_view{};
    if (!key.empty() && !open_quote) {
      out.push_back(Feature{std::string(key), std::string(value), {}});
      current = &out.back();
      in_qualifier = false;
      continue;
    }
    if (current == nullptr) continue;
    if (value.starts_with('/') && !open_quote) {
      const std::size_t eq = value.find('=');
      std::string name(value.substr(1, eq == std::string_view::npos ? std::string_view::npos : eq - 1));
      std::string text = eq == std::string_view::npos ? std::string() : std::string(value.substr(eq + 1));
      current->qualifiers.emplace_back(std::move(name), std::move(text));
      in_qualifier = true;
    } else if (in_qualifier) {
      std::string& text = current->qualifiers.back().second;
      const bool no_space = current->qualifiers.back().first == "translation" || text.ends_with('-');
      if (!no_space) text.push_back(' ');
      text.append(value);
    } else {
      current->location.append(value);
    }
    if (in_qualifier) {
      const std::string& text = current->qualifiers.back().second;
      std::size_t quotes = static_cast<std::size_t>(std::count(text.begin(), text.end(), '"'));
      open_quote = !text.empty() && text.front() == '"' && (quotes % 2 == 1);
    }
  }
  for (auto& feature : out) {
    for (auto& [key, value] : feature.qualifiers) value = strip_quotes(value);
  }
}

std::vector<std::string> wrap_words(std::string_view text, std::size_t width) {
  std::vector<std::string> lines;
  std::string line;
  for (const std::string_view word : words(text)) {
    if (!line.empty() && line.size() + 1 + word.size() > width) {
      lines.push_back(std::move(line));
      line.clear();
    }
    if (!line.empty()) line.push_back(' ');
    line.append(word);
    while (line.size() > width) {
      lines.push_back(line.substr(0, width));
      line.erase(0, width);
    }
  }
  if (!line.empty() || lines.empty()) lines.push_back(std::move(line));
  return lines;
}

std::vector<std::string> wrap_chars(std::string_view text, std::size_t width, char break_after) {
  std::vector<std::string> lines;
  while (text.size() > width) {
    std::size_t cut = width;
    if (break_after != 0) {
      const std::size_t at = text.substr(0, width).find_last_of(break_after);
      if (at != std::string_view::npos && at > 0) cut = at + 1;
    }
    lines.emplace_back(text.substr(0, cut));
    text.remove_prefix(cut);
  }
  lines.emplace_back(text);
  return lines;
}

/// Feature table lines without any prefix: key padded to 16, values at 16.
std::vector<std::string> feature_lines(const std::vector<Feature>& features) {
  std::vector<std::string> out;
  const std::string indent(16, ' ');
  for (const auto& f : features) {
    const auto location = wrap_chars(f.location, 58, ',');
    for (std::size_t i = 0; i < location.size(); ++i) {
      std::string key = i == 0 ? f.type : std::string();
      key.resize(16, ' ');
      out.push_back(key + location[i]);
    }
    for (const auto& [key, value] : f.qualifiers) {
      std::string text = "/" + key;
      const bool numeric = !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        return util::is_ascii_digit(c);
      });
      if (!value.empty()) {
        text += "=";
        text += numeric || key == "transl_table" || key == "codon_start" ? value
                                                                          : "\"" + util::replace_all(value, "\"", "\"\"") + "\"";
      }
      const auto wrapped = key == "translation" ? wrap_chars(text, 58, 0) : wrap_words(text, 58);
      for (const auto& line : wrapped) out.push_back(indent + line);
    }
  }
  return out;
}

std::string molecule_from_letters(const std::string& sequence) {
  std::size_t nucleotides = 0;
  bool has_u = false;
  for (const char c : sequence) {
    const char u = util::ascii_upper(c);
    if (u == 'A' || u == 'C' || u == 'G' || u == 'T' || u == 'U' || u == 'N') ++nucleotides;
    has_u = has_u || u == 'U';
  }
  if (!sequence.empty() && nucleotides * 10 < sequence.size() * 9) return "protein";
  return has_u ? "RNA" : "DNA";
}

Json string_list(std::string_view text, char separator) {
  Json list = Json::array();
  for (const std::string_view part : util::split(text, separator)) {
    const std::string_view item = util::trim(part);
    if (!item.empty() && item != ".") list.push_back(std::string(item));
  }
  return list;
}

std::string join_list(const Json& list, std::string_view separator) {
  std::string out;
  for (const Json& item : list.elements()) {
    if (!out.empty()) out += separator;
    out += item.as_string();
  }
  return out;
}

// --------------------------------------------------------------------------- GenBank

Result<std::vector<SeqRecord>> read_genbank(std::string_view text, ReadContext& ctx) {
  std::vector<SeqRecord> records;
  const auto lines = lines_of(text);
  std::size_t i = 0;
  while (i < lines.size()) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
    while (i < lines.size() && !lines[i].starts_with("LOCUS")) ++i;
    if (i >= lines.size()) break;
    SeqRecord r;
    const auto locus = words(lines[i].substr(5));
    if (!locus.empty()) r.name = std::string(locus[0]);
    for (std::size_t w = 1; w < locus.size(); ++w) {
      const std::string_view word = locus[w];
      if (word == "linear" || word == "circular") r.topology = std::string(word);
      if (word.find("DNA") != std::string_view::npos || word.find("RNA") != std::string_view::npos) {
        r.molecule_type = std::string(word);
      }
      if (word == "aa") r.molecule_type = "protein";
      if (word.size() == 3 && util::is_ascii_alpha(word[0]) && word == util::to_upper(word) && w + 1 == locus.size() - 1) {
        r.annotations["data_file_division"] = std::string(word);
      }
      if (w + 1 == locus.size() && word.size() == 11 && word[2] == '-') r.annotations["date"] = std::string(word);
    }
    ++i;
    std::string section;
    Json references = Json::array();
    std::vector<std::string_view> feature_block;
    bool in_features = false;
    while (i < lines.size() && !lines[i].starts_with("//")) {
      const std::string_view line = lines[i];
      if (line.starts_with("ORIGIN")) {
        ++i;
        while (i < lines.size() && !lines[i].starts_with("//")) r.sequence += upper_letters(lines[i++]);
        break;
      }
      if (line.starts_with("CONTIG")) {
        ++i;
        continue;
      }
      const std::string_view key = util::trim(line.substr(0, std::min<std::size_t>(line.size(), 12)));
      const std::string_view value = line.size() > 12 ? util::trim(line.substr(12)) : std::string_view{};
      const bool top_level = !line.empty() && !util::is_ascii_space(line[0]);
      if (top_level) in_features = line.starts_with("FEATURES");
      if (in_features) {
        if (!line.starts_with("FEATURES")) feature_block.push_back(line.size() > 5 ? line.substr(5) : std::string_view{});
        ++i;
        continue;
      }
      if (!key.empty()) {
        section = std::string(key);
        if (section == "REFERENCE") references.push_back(Json::object({{"reference", std::string(value)}}));
        const auto append_field = [&](Json& target, std::string_view field) {
          target[field] = std::string(value);
        };
        if (section == "DEFINITION") {
          r.description = std::string(value);
        } else if (section == "ACCESSION") {
          r.annotations["accessions"] = string_list(value, ' ');
        } else if (section == "VERSION") {
          const auto parts = words(value);
          if (!parts.empty()) r.id = std::string(parts[0]);
        } else if (section == "KEYWORDS") {
          append_field(r.annotations, "keywords");
        } else if (section == "SOURCE") {
          append_field(r.annotations, "source");
        } else if (section == "ORGANISM") {
          append_field(r.annotations, "organism");
          section = "TAXONOMY";
        } else if (section == "DBLINK") {
          append_field(r.annotations, "dblink");
        } else if (section == "COMMENT") {
          append_field(r.annotations, "comment");
        } else if (!references.empty() && (section == "AUTHORS" || section == "CONSRTM" || section == "TITLE" ||
                                            section == "JOURNAL" || section == "PUBMED" || section == "REMARK")) {
          references[references.size() - 1][util::to_lower(section)] = std::string(value);
        }
      } else {
        // Continuation line of the current section.
        const auto extend = [&](Json& target, std::string_view field, std::string_view sep) {
          std::string joined = target[field].as_string();
          if (!joined.empty()) joined += sep;
          joined += value;
          target[field] = std::move(joined);
        };
        if (section == "DEFINITION") {
          r.description += " ";
          r.description += value;
        } else if (section == "TAXONOMY") {
          extend(r.annotations, "taxonomy_text", " ");
        } else if (section == "KEYWORDS" || section == "SOURCE") {
          extend(r.annotations, util::to_lower(section), " ");
        } else if (section == "COMMENT") {
          extend(r.annotations, "comment", "\n");
        } else if (section == "ACCESSION") {
          for (const Json& extra : string_list(value, ' ').elements()) r.annotations["accessions"].push_back(extra);
        } else if (!references.empty() && (section == "AUTHORS" || section == "TITLE" || section == "JOURNAL" ||
                                            section == "CONSRTM" || section == "REMARK")) {
          extend(references[references.size() - 1], util::to_lower(section), " ");
        }
      }
      ++i;
    }
    if (i < lines.size()) ++i;  // "//"
    if (r.description.ends_with('.')) r.description.pop_back();
    if (const Json* tax = r.annotations.find("taxonomy_text")) {
      std::string t = tax->as_string();
      if (t.ends_with('.')) t.pop_back();
      r.annotations["taxonomy"] = string_list(t, ';');
      r.annotations.erase("taxonomy_text");
    }
    if (const Json* kw = r.annotations.find("keywords")) {
      std::string t = kw->as_string();
      if (t.ends_with('.')) t.pop_back();
      r.annotations["keywords"] = string_list(t, ';');
    }
    if (!references.empty()) r.annotations["references"] = std::move(references);
    parse_features(feature_block, r.features);
    if (r.id.empty()) r.id = r.name;
    if (r.molecule_type.empty()) r.molecule_type = molecule_from_letters(r.sequence);
    records.push_back(std::move(r));
    CVT_TRY(check_cells(ctx, records.size() * 3));
  }
  if (records.empty()) return fail(Status::Decode, "no GenBank LOCUS records found");
  return records;
}

void put_wrapped(std::string& out, std::string_view key, std::string_view value, std::size_t indent = 12) {
  const auto lines = wrap_words(value, 80 - indent);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    std::string prefix = i == 0 ? std::string(key) : std::string();
    prefix.resize(indent, ' ');
    out += prefix + lines[i] + '\n';
  }
}

Result<std::string> write_genbank(const std::vector<SeqRecord>& records, WriteContext& /*ctx*/) {
  std::string out;
  for (const auto& r : records) {
    const std::string molecule = r.molecule_type.empty() ? molecule_from_letters(r.sequence) : r.molecule_type;
    const bool protein = molecule == "protein";
    std::string name = r.name.empty() ? r.id : r.name;
    if (name.find('.') != std::string::npos && r.name.empty()) name = name.substr(0, name.find('.'));
    std::replace(name.begin(), name.end(), ' ', '_');
    const std::string division = r.annotations["data_file_division"].as_string().empty()
                                     ? "UNK"
                                     : r.annotations["data_file_division"].as_string();
    const std::string date = r.annotations["date"].as_string().empty() ? "01-JAN-1980" : r.annotations["date"].as_string();
    out += std::format("LOCUS       {:<16} {:>11} {}    {:<6}  {:<8} {} {}\n", name, r.sequence.size(),
                       protein ? "aa" : "bp", protein ? "" : molecule, r.topology.empty() ? "linear" : r.topology,
                       division, date);
    std::string definition = r.description.empty() ? "." : r.description;
    if (!definition.ends_with('.')) definition += '.';
    put_wrapped(out, "DEFINITION", definition);
    const Json& accessions = r.annotations["accessions"];
    std::string accession = accessions.is_array() && !accessions.empty() ? join_list(accessions, " ") : name;
    put_wrapped(out, "ACCESSION", accession);
    put_wrapped(out, "VERSION", r.id);
    if (const Json* dblink = r.annotations.find("dblink")) put_wrapped(out, "DBLINK", dblink->as_string());
    const Json& keywords = r.annotations["keywords"];
    put_wrapped(out, "KEYWORDS", (keywords.is_array() ? join_list(keywords, "; ") : keywords.as_string()) + ".");
    put_wrapped(out, "SOURCE", r.annotations["source"].as_string().empty() ? "." : r.annotations["source"].as_string());
    put_wrapped(out, "  ORGANISM",
                r.annotations["organism"].as_string().empty() ? "." : r.annotations["organism"].as_string());
    if (const Json* taxonomy = r.annotations.find("taxonomy"); taxonomy != nullptr && taxonomy->is_array()) {
      if (!taxonomy->empty()) put_wrapped(out, "", join_list(*taxonomy, "; ") + ".");
    }
    if (const Json* refs = r.annotations.find("references"); refs != nullptr && refs->is_array()) {
      for (const Json& ref : refs->elements()) {
        put_wrapped(out, "REFERENCE", ref["reference"].as_string());
        for (const std::string_view field : {"authors", "consrtm", "title", "journal", "pubmed", "remark"}) {
          if (const Json* v = ref.find(field)) put_wrapped(out, "  " + util::to_upper(field), v->as_string());
        }
      }
    }
    if (const Json* comment = r.annotations.find("comment")) {
      for (const std::string_view line : lines_of(comment->as_string())) put_wrapped(out, "COMMENT", line);
    }
    out += "FEATURES             Location/Qualifiers\n";
    for (const auto& line : feature_lines(r.features)) out += "     " + line + '\n';
    out += "ORIGIN\n";
    for (std::size_t at = 0; at < r.sequence.size(); at += 60) {
      out += std::format("{:>9}", at + 1);
      for (std::size_t block = at; block < std::min(at + 60, r.sequence.size()); block += 10) {
        out += ' ' + util::to_lower(r.sequence.substr(block, 10));
      }
      out += '\n';
    }
    out += "//\n";
  }
  return out;
}

int sniff_genbank(ByteView data, std::string_view /*name*/) {
  const std::string_view head = head_text(data, 1024);
  if (!head.starts_with("LOCUS ")) return 0;
  return head.find("\nDEFINITION") != std::string_view::npos || head.find(" bp ") != std::string_view::npos ? 92 : 60;
}

// --------------------------------------------------------------------------- EMBL

Result<std::vector<SeqRecord>> read_embl(std::string_view text, ReadContext& ctx) {
  std::vector<SeqRecord> records;
  const auto lines = lines_of(text);
  std::size_t i = 0;
  while (i < lines.size()) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
    while (i < lines.size() && !lines[i].starts_with("ID ")) ++i;
    if (i >= lines.size()) break;
    SeqRecord r;
    {
      const auto fields = util::split(lines[i].substr(5), ';');
      if (!fields.empty()) r.name = std::string(util::trim(fields[0]));
      for (std::size_t f = 1; f < fields.size(); ++f) {
        const std::string_view field = util::trim(fields[f]);
        if (field == "linear" || field == "circular") r.topology = std::string(field);
        if (field.find("DNA") != std::string_view::npos || field.find("RNA") != std::string_view::npos) {
          r.molecule_type = std::string(field);
        }
        if (f == 5 && field.size() == 3) r.annotations["data_file_division"] = std::string(field);
        if (field.starts_with("SV ")) r.annotations["sequence_version"] = std::string(field.substr(3));
      }
      if (r.name.empty()) r.name = "unnamed";
    }
    ++i;
    std::vector<std::string_view> feature_block;
    Json references = Json::array();
    std::string taxonomy;
    while (i < lines.size() && !lines[i].starts_with("//")) {
      const std::string_view line = lines[i];
      const std::string_view code = line.substr(0, std::min<std::size_t>(2, line.size()));
      const std::string_view value = line.size() > 5 ? util::trim(line.substr(5)) : std::string_view{};
      if (code == "SQ") {
        ++i;
        while (i < lines.size() && !lines[i].starts_with("//")) r.sequence += util::to_upper(upper_letters(lines[i++]));
        break;
      }
      const auto extend = [&](Json& target, std::string_view field, std::string_view sep) {
        std::string joined = target[field].as_string();
        if (!joined.empty()) joined += sep;
        joined += value;
        target[field] = std::move(joined);
      };
      if (code == "AC") {
        std::string t(value);
        if (t.ends_with(';')) t.pop_back();
        for (const Json& a : string_list(t, ';').elements()) {
          if (!r.annotations.contains("accessions")) r.annotations["accessions"] = Json::array();
          r.annotations["accessions"].push_back(a);
        }
      } else if (code == "DE") {
        if (!r.description.empty()) r.description += ' ';
        r.description += value;
      } else if (code == "KW") {
        const std::string previous = r.annotations["keywords"].as_string();
        extend(r.annotations, "keywords", previous.empty() || previous.ends_with(';') ? " " : "; ");
      } else if (code == "OS") {
        r.annotations["organism"] = std::string(value);
      } else if (code == "OC") {
        if (!taxonomy.empty()) taxonomy += ' ';
        taxonomy += value;
      } else if (code == "RN") {
        references.push_back(Json::object({{"reference", std::string(value)}}));
      } else if (!references.empty() && (code == "RA" || code == "RT" || code == "RL" || code == "RX")) {
        const std::string_view field = code == "RA" ? "authors" : code == "RT" ? "title" : code == "RL" ? "journal" : "xref";
        extend(references[references.size() - 1], field, " ");
      } else if (code == "FT") {
        feature_block.push_back(line.size() > 5 ? line.substr(5) : std::string_view{});
      } else if (code == "CC") {
        extend(r.annotations, "comment", "\n");
      }
      ++i;
    }
    if (i < lines.size()) ++i;
    if (r.description.ends_with('.')) r.description.pop_back();
    if (!taxonomy.empty()) {
      if (taxonomy.ends_with('.')) taxonomy.pop_back();
      r.annotations["taxonomy"] = string_list(taxonomy, ';');
    }
    if (const Json* kw = r.annotations.find("keywords")) {
      std::string t = kw->as_string();
      if (t.ends_with('.')) t.pop_back();
      r.annotations["keywords"] = string_list(t, ';');
    }
    for (Json& ref : references.elements()) {
      for (const std::string_view field : {"title", "authors"}) {
        if (Json* v = ref.find(field)) {
          std::string t = v->as_string();
          if (t.ends_with(';')) t.pop_back();
          if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
          *v = std::move(t);
        }
      }
    }
    if (!references.empty()) r.annotations["references"] = std::move(references);
    parse_features(feature_block, r.features);
    const Json& accessions = r.annotations["accessions"];
    r.id = accessions.is_array() && !accessions.empty() ? accessions[0].as_string() : r.name;
    if (const Json* sv = r.annotations.find("sequence_version")) r.id += "." + sv->as_string();
    if (r.molecule_type.empty()) r.molecule_type = molecule_from_letters(r.sequence);
    records.push_back(std::move(r));
    CVT_TRY(check_cells(ctx, records.size() * 3));
  }
  if (records.empty()) return fail(Status::Decode, "no EMBL ID records found");
  return records;
}

void put_embl(std::string& out, std::string_view code, std::string_view value) {
  for (const auto& line : wrap_words(value, 75)) out += std::string(code) + "   " + line + '\n';
}

Result<std::string> write_embl(const std::vector<SeqRecord>& records, WriteContext& /*ctx*/) {
  std::string out;
  for (const auto& r : records) {
    const std::string molecule = r.molecule_type.empty() ? molecule_from_letters(r.sequence) : r.molecule_type;
    std::string accession = r.id;
    std::string version = "1";
    if (const std::size_t dot = accession.rfind('.'); dot != std::string::npos) {
      version = accession.substr(dot + 1);
      accession.resize(dot);
    }
    const std::string division = r.annotations["data_file_division"].as_string().empty()
                                     ? "UNC"
                                     : r.annotations["data_file_division"].as_string();
    out += std::format("ID   {}; SV {}; {}; {}; STD; {}; {} BP.\nXX\n", accession, version,
                       r.topology.empty() ? "linear" : r.topology, molecule, division, r.sequence.size());
    const Json& accessions = r.annotations["accessions"];
    out += "AC   " + (accessions.is_array() && !accessions.empty() ? join_list(accessions, "; ") : accession) + ";\nXX\n";
    put_embl(out, "DE", r.description.empty() ? "." : r.description);
    out += "XX\n";
    const Json& keywords = r.annotations["keywords"];
    out += "KW   " + (keywords.is_array() ? join_list(keywords, "; ") : keywords.as_string()) + ".\nXX\n";
    if (const Json* organism = r.annotations.find("organism")) {
      put_embl(out, "OS", organism->as_string());
      if (const Json* taxonomy = r.annotations.find("taxonomy"); taxonomy != nullptr && taxonomy->is_array()) {
        put_embl(out, "OC", join_list(*taxonomy, "; ") + ".");
      }
      out += "XX\n";
    }
    if (const Json* refs = r.annotations.find("references"); refs != nullptr && refs->is_array()) {
      std::size_t n = 0;
      for (const Json& ref : refs->elements()) {
        out += std::format("RN   [{}]\n", ++n);
        if (const Json* v = ref.find("authors")) put_embl(out, "RA", v->as_string() + ";");
        if (const Json* v = ref.find("title")) put_embl(out, "RT", "\"" + v->as_string() + "\";");
        if (const Json* v = ref.find("journal")) put_embl(out, "RL", v->as_string());
        out += "XX\n";
      }
    }
    if (!r.features.empty()) {
      out += "FH   Key             Location/Qualifiers\nFH\n";
      for (const auto& line : feature_lines(r.features)) out += "FT   " + line + '\n';
      out += "XX\n";
    }
    std::size_t a = 0, c = 0, g = 0, t = 0;
    for (const char ch : r.sequence) {
      switch (util::ascii_upper(ch)) {
        case 'A': ++a; break;
        case 'C': ++c; break;
        case 'G': ++g; break;
        case 'T': ++t; break;
        default: break;
      }
    }
    out += std::format("SQ   Sequence {} BP; {} A; {} C; {} G; {} T; {} other;\n", r.sequence.size(), a, c, g, t,
                       r.sequence.size() - a - c - g - t);
    for (std::size_t at = 0; at < r.sequence.size(); at += 60) {
      std::string line = "    ";
      const std::size_t end = std::min(at + 60, r.sequence.size());
      for (std::size_t block = at; block < end; block += 10) line += ' ' + util::to_lower(r.sequence.substr(block, 10));
      line.resize(std::max<std::size_t>(line.size(), 70), ' ');
      out += line + std::format("{:>10}\n", end);
    }
    out += "//\n";
  }
  return out;
}

int sniff_embl(ByteView data, std::string_view /*name*/) {
  const std::string_view head = head_text(data, 1024);
  if (!head.starts_with("ID   ")) return 0;
  const std::string_view first = lines_of(head).front();
  if (first.find(" BP.") != std::string_view::npos) return 92;
  return 0;
}

// --------------------------------------------------------------------------- UniProt (Swiss-Prot text)

Result<std::vector<SeqRecord>> read_uniprot(std::string_view text, ReadContext& ctx) {
  std::vector<SeqRecord> records;
  const auto lines = lines_of(text);
  std::size_t i = 0;
  while (i < lines.size()) {
    CVT_TRY(ctx.checkpoint(static_cast<double>(i) / static_cast<double>(lines.size())));
    while (i < lines.size() && !lines[i].starts_with("ID ")) ++i;
    if (i >= lines.size()) break;
    SeqRecord r;
    r.molecule_type = "protein";
    const auto id_words = words(lines[i].substr(2));
    if (!id_words.empty()) r.name = std::string(id_words[0]);
    if (id_words.size() > 1) {
      std::string status(id_words[1]);
      if (status.ends_with(';')) status.pop_back();
      r.annotations["status"] = status;
    }
    ++i;
    std::string description;
    std::string taxonomy;
    std::vector<std::string_view> feature_block;
    Json comments = Json::array();
    while (i < lines.size() && !lines[i].starts_with("//")) {
      const std::string_view line = lines[i];
      const std::string_view code = line.substr(0, std::min<std::size_t>(2, line.size()));
      const std::string_view value = line.size() > 5 ? util::trim(line.substr(5)) : std::string_view{};
      if (code == "SQ") {
        const auto parts = util::split(value, ';');
        for (const auto part : parts) {
          const auto w = words(part);
          if (w.size() == 2 && w[1] == "MW") r.annotations["molecular_weight"] = json_number(to_double(w[0]).value_or(0));
          if (w.size() == 2 && w[1] == "CRC64") r.annotations["crc64"] = std::string(w[0]);
        }
        ++i;
        while (i < lines.size() && !lines[i].starts_with("//")) r.sequence += upper_letters(lines[i++]);
        break;
      }
      if (code == "AC") {
        for (const Json& a : string_list(value, ';').elements()) {
          if (!r.annotations.contains("accessions")) r.annotations["accessions"] = Json::array();
          r.annotations["accessions"].push_back(a);
        }
      } else if (code == "DE") {
        if (value.starts_with("RecName: Full=") && description.empty()) {
          description = std::string(value.substr(14));
          if (description.ends_with(';')) description.pop_back();
          if (const std::size_t brace = description.find(" {"); brace != std::string::npos) description.resize(brace);
        }
      } else if (code == "GN") {
        r.annotations["gene"] = std::string(value);
      } else if (code == "OS") {
        std::string organism = r.annotations["organism"].as_string();
        if (!organism.empty()) organism += ' ';
        organism += value;
        r.annotations["organism"] = std::move(organism);
      } else if (code == "OC") {
        if (!taxonomy.empty()) taxonomy += ' ';
        taxonomy += value;
      } else if (code == "OX") {
        r.annotations["taxonomy_id"] = std::string(value);
      } else if (code == "KW") {
        std::string kw = r.annotations["keywords_text"].as_string();
        if (!kw.empty()) kw += ' ';
        kw += value;
        r.annotations["keywords_text"] = std::move(kw);
      } else if (code == "CC") {
        if (value.starts_with("-!-")) {
          comments.push_back(std::string(util::trim(value.substr(3))));
        } else if (!comments.empty() && !value.starts_with("---") && !value.starts_with("Copyrighted") &&
                   !value.starts_with("Distributed")) {
          comments[comments.size() - 1] = comments[comments.size() - 1].as_string() + " " + std::string(value);
        }
      } else if (code == "FT") {
        feature_block.push_back(line.size() > 5 ? line.substr(5) : std::string_view{});
      }
      ++i;
    }
    if (i < lines.size()) ++i;
    const Json& accessions = r.annotations["accessions"];
    r.id = accessions.is_array() && !accessions.empty() ? accessions[0].as_string() : r.name;
    r.description = description;
    if (!taxonomy.empty()) {
      if (taxonomy.ends_with('.')) taxonomy.pop_back();
      r.annotations["taxonomy"] = string_list(taxonomy, ';');
    }
    if (const Json* organism = r.annotations.find("organism")) {
      std::string o = organism->as_string();
      if (o.ends_with('.')) o.pop_back();
      r.annotations["organism"] = std::move(o);
    }
    if (const Json* kw = r.annotations.find("keywords_text")) {
      std::string t = kw->as_string();
      if (t.ends_with('.')) t.pop_back();
      r.annotations["keywords"] = string_list(t, ';');
      r.annotations.erase("keywords_text");
    }
    if (!comments.empty()) r.annotations["comments"] = std::move(comments);
    // UniProt feature lines (new style): key at 0..15, location after.
    parse_features(feature_block, r.features);
    records.push_back(std::move(r));
    CVT_TRY(check_cells(ctx, records.size() * 3));
  }
  if (records.empty()) return fail(Status::Decode, "no UniProt ID records found");
  return records;
}

int sniff_uniprot(ByteView data, std::string_view /*name*/) {
  const std::string_view head = head_text(data, 1024);
  if (!head.starts_with("ID   ")) return 0;
  const std::string_view first = lines_of(head).front();
  return first.ends_with(" AA.") ? 92 : 0;
}

}  // namespace
}  // namespace cvt::science::bio

namespace cvt::science {

void register_sequences(Registry& registry) {
  using namespace bio;
  add_format(registry, {.id = "fasta",
                        .name = "FASTA sequences",
                        .description = "Nucleotide or protein sequences with '>' title lines.",
                        .extensions = {"fasta", "fa", "fna", "faa", "ffn", "frn", "fas", "mpfa"},
                        .mime = "text/x-fasta",
                        .popularity = 45,
                        .sniff = sniff_fasta});
  add_record_codecs(registry, {.id = "fasta",
                               .read = read_fasta,
                               .write = write_fasta,
                               .read_options = {},
                               .write_options = {OptionSpec::integer("line_width", "Line width", 60, 0, 100000,
                                                                     "Residues per line; 0 writes each sequence on one line.")},
                               .lossy_table = false});

  add_format(registry, {.id = "fastq",
                        .name = "FASTQ reads",
                        .description = "Sequencing reads with per-base quality scores.",
                        .extensions = {"fastq", "fq"},
                        .mime = "text/x-fastq",
                        .popularity = 35,
                        .sniff = sniff_fastq});
  add_record_codecs(
      registry,
      {.id = "fastq",
       .read = read_fastq,
       .write = write_fastq,
       .read_options = {OptionSpec::choice("quality_encoding_in", "Input quality encoding", "auto",
                                           {"auto", "phred33", "phred64", "solexa"},
                                           "How the input encodes quality scores; auto detects it from the characters used.")},
       .write_options = {OptionSpec::choice("quality_encoding", "Quality encoding", "phred33",
                                            {"phred33", "phred64", "solexa"},
                                            "Quality score encoding of the output (Sanger/Illumina 1.8+ = phred33)."),
                         OptionSpec::integer("default_quality", "Default quality", 40, 0, 93,
                                             "Phred score given to bases whose source has no quality values.")},
       .lossy_table = false});

  add_format(registry, {.id = "genbank",
                        .name = "GenBank flat file",
                        .description = "NCBI GenBank sequence record with annotations and features.",
                        .extensions = {"gb", "gbk", "genbank", "gbff"},
                        .mime = "chemical/x-genbank",
                        .popularity = 30,
                        .sniff = sniff_genbank});
  add_record_codecs(registry, {.id = "genbank", .read = read_genbank, .write = write_genbank, .lossy_table = true});

  add_format(registry, {.id = "embl",
                        .name = "EMBL flat file",
                        .description = "EMBL/ENA nucleotide sequence record with annotations and features.",
                        .extensions = {"embl", "emb"},
                        .mime = "chemical/x-embl-dl-nucleotide",
                        .popularity = 18,
                        .sniff = sniff_embl});
  add_record_codecs(registry, {.id = "embl", .read = read_embl, .write = write_embl, .lossy_table = true});

  add_format(registry, {.id = "uniprot",
                        .name = "UniProt flat file",
                        .description = "UniProtKB/Swiss-Prot protein entry in text format.",
                        .extensions = {"swiss", "sprot", "uniprot"},
                        .mime = "text/plain",
                        .popularity = 15,
                        .sniff = sniff_uniprot});
  add_record_codecs(registry, {.id = "uniprot", .read = read_uniprot, .write = nullptr, .lossy_table = true});
}

}  // namespace cvt::science

// Convertible core - science family: the sequence record model shared by the
// bioinformatics formats (FASTA, FASTQ, GenBank, EMBL, UniProt, alignments).
//
// As a Table: one sheet "sequences" with the columns id, description,
// sequence and (when any record has qualities) quality, Phred+33 encoded.
// As Data: {"records":[{"id","name","description","molecule_type","topology",
// "sequence","quality","annotations":{...},"features":[{"type","location",
// "qualifiers":{...}}]}]}.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "families/science/common.hpp"

namespace cvt::science::bio {

struct Feature {
  std::string type;
  std::string location;
  std::vector<std::pair<std::string, std::string>> qualifiers;
};

struct SeqRecord {
  std::string id{};
  std::string name;
  std::string description;
  std::string sequence;
  std::string quality;  ///< Phred+33 characters, same length as sequence, or empty
  std::string molecule_type;
  std::string topology;
  Json annotations = Json::object();
  std::vector<Feature> features;
};

[[nodiscard]] ir::Table records_to_table(const std::vector<SeqRecord>& records);
[[nodiscard]] Result<std::vector<SeqRecord>> table_to_records(const ir::Table& table);
[[nodiscard]] Json records_to_json(const std::vector<SeqRecord>& records);
[[nodiscard]] Result<std::vector<SeqRecord>> json_to_records(const Json& value);

/// Register Table and Data codecs for a format whose content is a list of
/// sequence records.
struct RecordCodec {
  std::string id{};
  std::function<Result<std::vector<SeqRecord>>(std::string_view text, ReadContext& ctx)> read{};
  std::function<Result<std::string>(const std::vector<SeqRecord>& records, WriteContext& ctx)> write{};
  std::vector<OptionSpec> read_options{};
  std::vector<OptionSpec> write_options{};
  bool lossy_table = false;  ///< the Table view drops annotations/features
};
void add_record_codecs(Registry& registry, RecordCodec codec);

}  // namespace cvt::science::bio

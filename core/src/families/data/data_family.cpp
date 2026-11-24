// Convertible core - data family.
// Structured data (json, yaml, toml, xml, ini, plist, msgpack, cbor, ...).
// Every format reads into and writes from ir::Data; see data.hpp for the
// tagged-value conventions that carry binary data, dates and big numbers.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/data/data.hpp"
#include "families/families.hpp"

namespace cvt {

void register_data_family(Registry& registry) {
  data::register_json_formats(registry);
  data::register_yaml(registry);
  data::register_toml(registry);
  data::register_xml_data(registry);
  data::register_plist(registry);
  data::register_config_formats(registry);
  data::register_web_formats(registry);
  data::register_msgpack(registry);
  data::register_cbor(registry);
  data::register_bson(registry);
  data::register_ubjson(registry);
  data::register_smile(registry);
  data::register_ion(registry);
  data::register_bencode(registry);
  data::register_protobuf(registry);
  data::register_avro(registry);
  data::register_flexbuffers(registry);
  data::register_pickle(registry);
  data::register_hjson(registry);
  data::register_lisp_formats(registry);
  data::register_kdl(registry);
  data::register_hocon(registry);
  data::register_ron(registry);
  data::register_cson(registry);
  data::register_script_literals(registry);
  data::register_data_bridges(registry);
}

}  // namespace cvt

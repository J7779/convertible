# Sources: data

Fixtures of the data family (`core/tests/families/data/`). Every file was
written by an independent, widely used library or tool from one synthetic
dataset (`tools/dataset.py`: made-up strings including non-ASCII text and an
emoji, 64-bit extremes, special floats, empty containers, deep nesting, a
16-byte binary pattern `00 11 22 ... ff 00 ff`, the date 2024-05-01T12:30:45.123456Z).
There is no personal or machine data in any file.

Each `<name>.<ext>` has a `<name>.expected.json` next to it (one file,
`pickle.expected.json`, serves all six pickles; `jackson_smile.expected.json`
both Smile files): the same data written by Python's `json` module in the
data family's tagged-JSON convention (`{"$binary": ...}`, `{"$date": ...}`,
`{"$bigint": ...}`, `{"$decimal": ...}`, `{"$float": "NaN"}`, ...). The test
`data/fixtures` reads each fixture with our reader and compares the result
with the expected JSON value by value (`test::data_equal`, exact). The two
`$decimal` spellings in `cbor_python.expected.json` and
`bson_pymongo.expected.json` were changed by hand from the exponent form the
generator wrote to the equivalent positional form (same value).

## How the files were made

All Python-generated files come from one script, run in a scratch virtual
environment with the packages below installed:

```sh
python3 -m venv venv
venv/bin/pip install msgpack cbor2 pymongo tomli-w protobuf fastavro amazon.ion PyYAML ruamel.yaml \
    flatbuffers json5 hjson bencode.py edn_format kdl-py pyhocon py-ubjson bjdata phpserialize sexpdata jproperties
venv/bin/python -I tools/make_fixtures.py out
```

`tools/make_fixtures.py` (with `tools/dataset.py`) is committed here; the
command for each file is the call in that script that is named in the table.
Python 3.14.10 with: msgpack 1.2.4, cbor2 6.3.0, pymongo 4.20.1, tomli_w 1.2.0,
protobuf 7.36.2, fastavro 1.13.1, amazon-ion 0.16.0, PyYAML 6.0.4,
ruamel.yaml 0.19.1, flatbuffers 25.12.19, json5 0.17.3, hjson 3.1.0,
bencode.py 4.1.0, edn_format 0.8.0, kdl-py 1.5.0, pyhocon 0.3.63,
py-ubjson 0.16.1, bjdata 0.6.6, phpserialize 1.3, sexpdata 1.0.2,
jproperties 2.1.2.

| File | Contents | Tool | Command (in `tools/make_fixtures.py` unless stated) |
| --- | --- | --- | --- |
| `python_json_pretty.json` | dataset without specials, UTF-8, indented | Python `json` | `json.dumps(plain, indent=2, ensure_ascii=False)` |
| `python_json_ascii.json` | same, `\uXXXX` escapes and surrogate pairs | Python `json` | `json.dumps(plain, ensure_ascii=True)` |
| `python_json_nan.json` | full dataset with `NaN`/`Infinity` literals | Python `json` | `json.dumps(core)` |
| `python_lines.ndjson` | five records, one per line | Python `json` | `json.dumps(r) + "\n"` per record |
| `json5_python.json5` | small object with `Infinity` | json5 | `json5.dumps(..., indent=2)` |
| `hjson_python.hjson` | dataset without specials | hjson | `hjson.dumps(plain)` |
| `pyyaml_default.yaml` | dataset + `!!binary`, timestamp, date, literal block | PyYAML | `yaml.dump(ym, allow_unicode=True, sort_keys=False)` |
| `pyyaml_flow.yaml` | dataset in flow style, ASCII escapes | PyYAML | `yaml.dump(..., default_flow_style=True, allow_unicode=False)` |
| `pyyaml_multi.yaml` | three documents | PyYAML | `yaml.dump_all([...], explicit_start=True)` |
| `ruamel_default.yaml` | dataset, block style, multi-line string | ruamel.yaml | `YAML().dump(rd, buf)` |
| `tomli_w.toml` | dataset without null; offset/local dates and times; array of tables | tomli-w | `tomli_w.dumps(tm, multiline_strings=True)` |
| `plistlib_xml.plist` | dataset without null; `<data>`, `<date>` | Python `plistlib` | `plistlib.dumps(pl, fmt=FMT_XML, sort_keys=False)` |
| `plistlib_binary.plist` | same plus a UID, `bplist00` | Python `plistlib` | `plistlib.dumps(pl, fmt=FMT_BINARY, sort_keys=False)` |
| `plutil_binary.plist` | `plistlib_xml.plist` re-encoded by Apple's Core Foundation | macOS `plutil` | `plutil -convert binary1 -o plutil_binary.plist plistlib_xml.plist` (expected JSON copied from `plistlib_xml`) |
| `plutil_xml.plist` | `plistlib_binary.plist` re-encoded as XML (17-digit reals, CF$UID) | macOS `plutil` | `plutil -convert xml1 -o plutil_xml.plist plistlib_binary.plist` (expected JSON copied from `plistlib_binary`) |
| `configparser.ini` | three sections, a multi-line value, an empty section | Python `configparser` | `ConfigParser(interpolation=None).write(buf)` |
| `jproperties.properties` | dotted keys, `é` escapes, `\=`, backslashes | jproperties | `props.store(buf, encoding="latin-1", timestamp=False)` |
| `elementtree.xml` | catalog with attributes, repeated and empty elements | Python `xml.etree.ElementTree` | `ET.tostring(root, encoding="unicode", xml_declaration=True)` after `ET.indent` |
| `msgpack_python.msgpack` | dataset + bin, timestamp extension, float | msgpack | `msgpack.packb(mp, datetime=True, use_bin_type=True)` |
| `msgpack_ext.msgpack` | ext type 5; 32-bit and 96-bit timestamps | msgpack | `msgpack.packb(ext, datetime=True)` |
| `cbor_python.cbor` | dataset + bytes, tag 0 date, bignums, decimal fraction, undefined, custom tag, half float | cbor2 | `cbor2.dumps(cb, datetime_as_timestamp=False)` |
| `cbor_canonical.cbor` | small map in canonical (sorted, shortest) form | cbor2 | `cbor2.dumps(..., canonical=True)` |
| `cbor_indefinite.cbor` | four indefinite-length items (map, nested arrays, chunked bytes and text) in the style of RFC 8949 Appendix A; checked inline in `fixtures_test.cpp` | hand-written hex | `bytes.fromhex("bf6346756ef563416d7421ff" "9f018202039f0405ffff" "5f42010243030405ff" "7f657374726561646d696e67ff")` |
| `bson_pymongo.bson` | dataset without uint64 + Binary (subtypes 0 and 4), datetime, ObjectId, Decimal128, Regex, Timestamp, Int64 | pymongo `bson` | `bson.encode(bs, codec_options=CodecOptions(tz_aware=True))` |
| `ubjson_python.ubj` | dataset + bytes (as `[$U#` array) | py-ubjson | `ubjson.dumpb(ub)` |
| `ubjson_python_optimised.ubj` | counted containers | py-ubjson | `ubjson.dumpb(..., container_count=True)` |
| `bjdata_python.bjd` | dataset incl. uint64 + bytes | bjdata | `bjdata.dumpb(bj)` |
| `jackson_default.sml` | `python_json_pretty.json` plus a 16-byte binary field | Jackson 2.17.2 `jackson-dataformat-smile`, OpenJDK 21 (`eclipse-temurin:21-jdk` container) | `tools/run_smile.sh encode` (runs `tools/SmileTool.java encode tools/smile_input.json jackson_default.sml`) |
| `jackson_shared.sml` | same with shared key names and shared string values | as above | `SmileTool.java encode smile_input.json jackson_shared.sml shared` |
| `ion_python.ion` | dataset + blob, timestamp, big integer, decimal (Ion text) | amazon-ion | `simpleion.dumps(io_data, binary=False)` |
| `ion_python.10n` | same as Ion binary (local symbol table) | amazon-ion | `simpleion.dumps(io_data, binary=True)` |
| `fastavro_null.avro` | two records: long, string, union, array, map, enum, bytes, timestamp-micros, date, decimal(10,2), boolean, float | fastavro | `fastavro.writer(buf, schema, rows, codec="null")` (random sync marker) |
| `fastavro_deflate.avro` | same with deflate-compressed blocks | fastavro | `fastavro.writer(buf, schema, rows, codec="deflate")` |
| `bencode_torrent.torrent` | torrent-like dictionary with 20 binary piece bytes | bencode.py | `bencode.encode(torrent)` |
| `flexbuffers_python.flexbuf` | map with int, big int, float, bool, null, typed and mixed vectors, nested map | flatbuffers (Python `flexbuffers`) | `flexbuffers.Dumps(fx)` |
| `protobuf_descriptor.pb` | a `FileDescriptorProto` ("example.proto", package "demo", message "Point" with two fields); checked inline | protobuf (Python) | `fd.SerializeToString()` |
| `pickle_protocol0..5.pickle` | dataset + bytes, tuple, shared list, 2**100 | Python `pickle` | `pickle.dumps(pk, protocol=p)` for p = 0..5 |
| `edn_python.edn` | keyword map | edn_format | `edn_format.dumps({...})` |
| `php_serialize.txt` | PHP array with nested list and UTF-8 bytes | phpserialize | `phpserialize.dumps(php)` |
| `python_repr.pyon` | dataset + bytes and tuple as Python literals | Python `pprint` | `pprint.pformat(pyl, width=100)` |
| `kdl_python.kdl` | nodes with arguments, properties and children (KDL 2) | kdl-py | `kdl.parse(...).print()` |
| `sexpdata.sexp` | nested list of symbols, numbers and a string | sexpdata | `sexpdata.dumps([...])` |
| `pyhocon.conf` | nested object with a list and a resolved substitution | pyhocon | `HOCONConverter.to_hocon(ConfigFactory.parse_string(...))` |

The Smile container run was:

```sh
docker run --rm --memory 2g -v "$PWD/tools":/w eclipse-temurin:21-jdk bash /w/run_smile.sh encode
```

(`run_smile.sh` downloads the four Jackson 2.17.2 jars from Maven Central
into the mounted directory first; they are not committed.)

## Interoperability of our output

Checked while developing (scratch scripts, not part of the build): the
synthetic dataset was converted from JSON to each format with the
`convertible` CLI and read back by the independent implementation, then
compared value by value. All passed:

| Our output | Read back by | Result |
| --- | --- | --- |
| json (pretty; minified, sorted, ASCII-only, NaN literals) | Python `json` | identical |
| yaml (block, flow, 46 strings that need quoting) | PyYAML 6.0.4 `safe_load`, ruamel.yaml 0.19.1 (safe) | identical, incl. `!!binary` and timestamps |
| toml | Python 3.14 `tomllib` | identical (null dropped by design) |
| plist, bplist | `plistlib`; `plutil -lint` | identical / OK |
| msgpack | msgpack 1.2.4 (`timestamp=3`) | identical incl. bin and timestamp |
| cbor | cbor2 6.3.0 | identical incl. tag 0 dates, bignums (tags 2/3), decimal fractions (tag 4) |
| bson | pymongo 4.20.1 | identical incl. ObjectId, Decimal128, Regex, Timestamp, datetime |
| ubjson, bjdata | py-ubjson 0.16.1, bjdata 0.6.6 | identical incl. binary |
| smile | Jackson 2.17.2 (`SmileTool.java decode`) | identical incl. BigInteger and BigDecimal tokens, long strings |
| ion (text) | amazon-ion 0.16.0 | identical incl. blob and timestamp |
| avro | fastavro 1.13.1 | identical incl. inferred union with null, timestamp-micros, bytes |
| bencode | bencode.py 4.1.0 | identical incl. binary strings |
| edn, hjson, json5 | edn_format, hjson, json5 | identical |
| php-serialized | phpserialize 1.3 | identical |
| python-literal | `ast.literal_eval` | identical (infinity as 1e999; NaN has no literal) |
| kdl, sexp | kdl-py 1.5.0, sexpdata 1.0.2 | parsed; nodes and values as written |
| ini | `configparser` | identical sections and values |
| querystring | `urllib.parse.parse_qs` | identical |
| xml | `xml.etree.ElementTree` | identical structure |

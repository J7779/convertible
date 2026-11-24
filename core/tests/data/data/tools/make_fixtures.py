"""Write fixtures with independent libraries, plus the expected tagged JSON.

Usage (from this directory, in a virtual environment with the libraries listed in
SOURCES.md): python3 -I tools/make_fixtures.py <out_dir>
Every fixture <name>.<ext> gets <name>.expected.json (the same data in the
tagged-JSON convention) unless noted.
"""
import base64
import configparser
import datetime
import decimal
import io
import json
import math
import os
import pickle
import plistlib
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dataset  # noqa: E402

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def save(name, data, expected=None):
    mode = "wb" if isinstance(data, (bytes, bytearray)) else "w"
    with open(os.path.join(out, name), mode, **({} if mode == "wb" else {"encoding": "utf-8", "newline": ""})) as f:
        f.write(data)
    if expected is not None:
        stem = name.rsplit(".", 1)[0]
        with open(os.path.join(out, stem + ".expected.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(dataset.tagged(expected), f, ensure_ascii=False, indent=1)
            f.write("\n")


core = dataset.core()

# ---- JSON (Python json)
plain = dataset.core(specials=False)
save("python_json_pretty.json", json.dumps(plain, indent=2, ensure_ascii=False) + "\n", plain)
save("python_json_ascii.json", json.dumps(plain, ensure_ascii=True), plain)
save("python_json_nan.json", json.dumps(core), core)  # NaN / Infinity literals (Python extension)

# ---- MessagePack
import msgpack  # noqa: E402

mp = dict(core)
mp["binary"] = dataset.BINARY
mp["date"] = dataset.DATE
mp["float32"] = 1.5
save("msgpack_python.msgpack", msgpack.packb(mp, datetime=True, use_bin_type=True), mp)
ext = {"ext": msgpack.ExtType(5, b"\x01\x02\x03"), "ts32": datetime.datetime(2020, 1, 1, tzinfo=dataset.UTC),
       "ts96": datetime.datetime(1960, 1, 1, 0, 0, 0, 1, tzinfo=dataset.UTC)}
save("msgpack_ext.msgpack", msgpack.packb(ext, datetime=True),
     {"ext": {"$ext": 5, "$binary": base64.b64encode(b"\x01\x02\x03").decode()},
      "ts32": ext["ts32"], "ts96": ext["ts96"]})

# ---- CBOR
import cbor2  # noqa: E402

cb = dict(core)
cb["binary"] = dataset.BINARY
cb["date"] = dataset.DATE
cb["bignum"] = 2**100
cb["neg_bignum"] = -(2**100)
cb["decimal"] = decimal.Decimal("3.14159265358979323846264338327950288")
cb["undefined"] = cbor2.undefined
cb["tagged"] = cbor2.CBORTag(1234, "custom")
cb["half"] = 1.5
expected_cb = dict(cb)
expected_cb["decimal"] = {"$decimal": "3.14159265358979323846264338327950288"}
expected_cb["bignum"] = {"$bigint": str(2**100)}
expected_cb["neg_bignum"] = {"$bigint": str(-(2**100))}
expected_cb["undefined"] = {"$undefined": True}
expected_cb["tagged"] = {"$tag": 1234, "$value": "custom"}
save("cbor_python.cbor", cbor2.dumps(cb, datetime_as_timestamp=False, canonical=False), expected_cb)
save("cbor_canonical.cbor", cbor2.dumps({"b": [1, 2.5, -3], "a": "x"}, canonical=True), {"a": "x", "b": [1, 2.5, -3]})
# Indefinite-length items written by hand in RFC 8949 Appendix A style.
save("cbor_indefinite.cbor", bytes.fromhex("bf6346756ef563416d7421ff") + bytes.fromhex("9f018202039f0405ffff")
     + bytes.fromhex("5f42010243030405ff") + bytes.fromhex("7f657374726561646d696e67ff"),
     None)

# ---- BSON
import bson  # noqa: E402
from bson import Binary, Decimal128, Int64, ObjectId, Regex, Timestamp  # noqa: E402

bs = dataset.core(uint64=False)
bs["binary"] = Binary(dataset.BINARY)
bs["uuid_binary"] = Binary(bytes(range(16)), 4)
bs["date"] = datetime.datetime(2024, 5, 1, 12, 30, 45, 123000, tzinfo=dataset.UTC)
bs["object_id"] = ObjectId("65f1a2b3c4d5e6f708192a3b")
bs["decimal128"] = Decimal128("12345.6789012345678901234567890123")
bs["regex"] = Regex("^ab+c$", "im")
bs["timestamp"] = Timestamp(1700000000, 7)
bs["int64_small"] = Int64(5)
expected_bs = dict(bs)
expected_bs["uuid_binary"] = {"$binary": base64.b64encode(bytes(range(16))).decode(), "$subtype": 4}
expected_bs["object_id"] = {"$oid": "65f1a2b3c4d5e6f708192a3b"}
expected_bs["decimal128"] = {"$decimal": "12345.6789012345678901234567890123"}
expected_bs["regex"] = {"$regex": "^ab+c$", "$options": "im"}
expected_bs["timestamp"] = {"$timestamp": {"t": 1700000000, "i": 7}}
expected_bs["int64_small"] = 5
expected_bs["date"] = {"$date": "2024-05-01T12:30:45.123Z"}
save("bson_pymongo.bson", bson.encode(bs, codec_options=bson.CodecOptions(tz_aware=True)), expected_bs)

# ---- UBJSON / BJData
import ubjson  # noqa: E402

ub = dataset.core(uint64=False, specials=False)
ub["binary"] = dataset.BINARY
save("ubjson_python.ubj", ubjson.dumpb(ub), ub)
save("ubjson_python_optimised.ubj", ubjson.dumpb({"ints": [1, 2, 3], "strs": ["a", "b"]}, container_count=True),
     {"ints": [1, 2, 3], "strs": ["a", "b"]})
import bjdata  # noqa: E402

bj = dataset.core(uint64=True, specials=False)
bj["binary"] = dataset.BINARY
save("bjdata_python.bjd", bjdata.dumpb(bj), bj)

# ---- YAML
import yaml  # noqa: E402

ym = dataset.core(specials=True)
ym["binary"] = dataset.BINARY
ym["date"] = dataset.DATE
ym["day"] = datetime.date(2024, 5, 1)
ym["multiline"] = "line one\nline two\n  indented\n"
save("pyyaml_default.yaml", yaml.dump(ym, allow_unicode=True, sort_keys=False), ym)
save("pyyaml_flow.yaml", yaml.dump(dataset.core(specials=False), default_flow_style=True, allow_unicode=False),
     dataset.core(specials=False))
save("pyyaml_multi.yaml", yaml.dump_all([{"doc": 1}, {"doc": 2}, [3, 4]], explicit_start=True),
     [{"doc": 1}, {"doc": 2}, [3, 4]])
from ruamel.yaml import YAML  # noqa: E402

ry = YAML()
ry.default_flow_style = False
buf = io.StringIO()
rd = dataset.core(specials=False)
rd["multiline"] = "line one\nline two\n"
ry.dump(rd, buf)
save("ruamel_default.yaml", buf.getvalue(), rd)

# ---- TOML (tomli-w)
import tomli_w  # noqa: E402

tm = dataset.core(uint64=False)
del tm["null"]
tm["list"] = [1, 2, 3]
tm["date"] = dataset.DATE
tm["local_datetime"] = datetime.datetime(2024, 5, 1, 12, 30, 45)
tm["local_date"] = datetime.date(2024, 5, 1)
tm["local_time"] = datetime.time(12, 30, 45, 500000)
tm["multiline"] = "line one\nline two\n"
tm["records"] = [{"id": 1, "name": "alpha"}, {"id": 2, "name": "beta"}]
expected_tm = dict(tm)
expected_tm["local_time"] = {"$date": "12:30:45.500000"}
expected_tm["local_datetime"] = {"$date": "2024-05-01T12:30:45"}
save("tomli_w.toml", tomli_w.dumps(tm, multiline_strings=True), expected_tm)

# ---- Property lists (plistlib)
pl = dataset.core(specials=False, uint64=False)
del pl["null"]
pl["list"] = [1, "two", 3.25, True]
pl["binary"] = dataset.BINARY
pl["date"] = datetime.datetime(2024, 5, 1, 12, 30, 45)
expected_pl = dict(pl)
expected_pl["date"] = {"$date": "2024-05-01T12:30:45Z"}
save("plistlib_xml.plist", plistlib.dumps(pl, fmt=plistlib.FMT_XML, sort_keys=False), expected_pl)
pl["uid"] = plistlib.UID(42)
expected_pl["uid"] = {"$uid": 42}
save("plistlib_binary.plist", plistlib.dumps(pl, fmt=plistlib.FMT_BINARY, sort_keys=False), expected_pl)

# ---- INI (configparser)
cp = configparser.ConfigParser(interpolation=None)
cp["server"] = {"host": "example.invalid", "port": "8080", "path": "/srv/data"}
cp["empty section"] = {}
cp["multi"] = {"text": "first line\nsecond line"}
buf = io.StringIO()
cp.write(buf)
save("configparser.ini", buf.getvalue(),
     {"server": {"host": "example.invalid", "port": "8080", "path": "/srv/data"}, "empty section": {},
      "multi": {"text": "first line\nsecond line"}})

# ---- Ion
from amazon.ion import simpleion  # noqa: E402

io_data = dataset.core(uint64=False, specials=False)
io_data["binary"] = dataset.BINARY
io_data["date"] = dataset.DATE
io_data["bigint"] = 2**100
io_data["decimal"] = decimal.Decimal("1.25")
expected_ion = dict(io_data)
expected_ion["bigint"] = {"$bigint": str(2**100)}
expected_ion["decimal"] = 1.25
save("ion_python.ion", simpleion.dumps(io_data, binary=False), expected_ion)
save("ion_python.10n", simpleion.dumps(io_data, binary=True), expected_ion)

# ---- Avro (fastavro)
import fastavro  # noqa: E402

schema = {
    "type": "record", "name": "Row", "namespace": "demo",
    "fields": [
        {"name": "id", "type": "long"},
        {"name": "name", "type": "string"},
        {"name": "score", "type": ["null", "double"], "default": None},
        {"name": "tags", "type": {"type": "array", "items": "string"}},
        {"name": "attrs", "type": {"type": "map", "values": "int"}},
        {"name": "kind", "type": {"type": "enum", "name": "Kind", "symbols": ["A", "B"]}},
        {"name": "blob", "type": "bytes"},
        {"name": "when", "type": {"type": "long", "logicalType": "timestamp-micros"}},
        {"name": "day", "type": {"type": "int", "logicalType": "date"}},
        {"name": "amount", "type": {"type": "bytes", "logicalType": "decimal", "precision": 10, "scale": 2}},
        {"name": "flag", "type": "boolean"},
        {"name": "ratio", "type": "float"},
    ],
}
rows = [
    {"id": 1, "name": "alpha é", "score": 1.5, "tags": ["x", "y"], "attrs": {"a": 1}, "kind": "A",
     "blob": b"\x00\x01\x02", "when": dataset.DATE, "day": datetime.date(2024, 5, 1),
     "amount": decimal.Decimal("123.45"), "flag": True, "ratio": 0.25},
    {"id": -2, "name": "beta", "score": None, "tags": [], "attrs": {}, "kind": "B", "blob": b"",
     "when": datetime.datetime(1969, 12, 31, 23, 59, 59, tzinfo=dataset.UTC), "day": datetime.date(1970, 1, 1),
     "amount": decimal.Decimal("-0.01"), "flag": False, "ratio": -2.0},
]
expected_rows = [dict(r) for r in rows]
expected_rows[0]["amount"] = 123.45
expected_rows[1]["amount"] = -0.01
for codec in ("null", "deflate"):
    buf = io.BytesIO()
    fastavro.writer(buf, fastavro.parse_schema(schema), rows, codec=codec, sync_interval=100)
    save(f"fastavro_{codec}.avro", buf.getvalue(), expected_rows)

# ---- bencode
import bencode  # noqa: E402

torrent = {"announce": "http://tracker.example.invalid/announce",
           "info": {"name": "sample.txt", "length": 1024, "piece length": 16384, "pieces": bytes(range(20))}}
save("bencode_torrent.torrent", bencode.encode(torrent), torrent)

# ---- FlexBuffers
from flatbuffers import flexbuffers  # noqa: E402

fx = {"string": "hello", "int": -12345, "big": 2**40, "float": 1.5, "bool": True, "null": None,
      "list": [1, 2, 3], "mixed": [1, "two", 3.5], "nested": {"a": {"b": [True, False]}},
      "unicode": "é✓"}
save("flexbuffers_python.flexbuf", bytes(flexbuffers.Dumps(fx)), fx)

# ---- Protocol Buffers (a FileDescriptorProto as a real-world message)
from google.protobuf import descriptor_pb2  # noqa: E402

fd = descriptor_pb2.FileDescriptorProto()
fd.name = "example.proto"
fd.package = "demo"
m = fd.message_type.add()
m.name = "Point"
f = m.field.add()
f.name = "x"
f.number = 1
f.type = descriptor_pb2.FieldDescriptorProto.TYPE_SINT32
f.label = descriptor_pb2.FieldDescriptorProto.LABEL_OPTIONAL
f = m.field.add()
f.name = "y"
f.number = 2
f.type = descriptor_pb2.FieldDescriptorProto.TYPE_DOUBLE
f.label = descriptor_pb2.FieldDescriptorProto.LABEL_OPTIONAL
f.default_value = "-1.5"
save("protobuf_descriptor.pb", fd.SerializeToString())

# ---- pickle (protocols 0..5)
pk = dataset.core(uint64=True, specials=True)
pk["bytes"] = b"\x00\x01binary"
pk["tuple"] = (1, 2)
pk["set_like"] = [3]
pk["big"] = 2**100
pk["shared"] = pk["list"]
expected_pk = dict(pk)
expected_pk["big"] = {"$bigint": str(2**100)}
for protocol in range(0, 6):
    save(f"pickle_protocol{protocol}.pickle", pickle.dumps(pk, protocol=protocol),
         expected_pk)

# ---- EDN, KDL, HOCON, PHP, S-expressions, Hjson, JSON5
import edn_format  # noqa: E402

edn_data = edn_format.dumps({edn_format.Keyword("name"): "alpha", edn_format.Keyword("count"): 3,
                             edn_format.Keyword("tags"): ["x", "y"], edn_format.Keyword("ok"): True,
                             edn_format.Keyword("ratio"): 0.5, edn_format.Keyword("none"): None})
save("edn_python.edn", edn_data, {"name": "alpha", "count": 3, "tags": ["x", "y"], "ok": True, "ratio": 0.5, "none": None})
import phpserialize  # noqa: E402

php = {b"name": b"alpha", b"count": 3, b"ratio": 0.5, b"ok": True, b"none": None, b"list": {0: b"x", 1: b"y"},
       b"unicode": "é".encode()}
save("php_serialize.txt", phpserialize.dumps(php),
     {"name": "alpha", "count": 3, "ratio": 0.5, "ok": True, "none": None, "list": ["x", "y"], "unicode": "é"})
import hjson  # noqa: E402

save("hjson_python.hjson", hjson.dumps(plain), plain)
import json5  # noqa: E402

save("json5_python.json5", json5.dumps({"a": 1, "b": [1, 2], "c": "x", "inf": math.inf}, indent=2),
     {"a": 1, "b": [1, 2], "c": "x", "inf": math.inf})

# ---- NDJSON (one json.dumps per line)
records = [{"id": i, "name": f"item {i}", "ok": i % 2 == 0, "score": i * 1.5} for i in range(5)]
save("python_lines.ndjson", "".join(json.dumps(r) + "\n" for r in records), records)

# ---- Python literal (repr / pprint)
import pprint  # noqa: E402

pyl = dataset.core(specials=False)
pyl["bytes"] = b"\x00\x01binary"
pyl["tuple"] = (1, 2)
save("python_repr.pyon", pprint.pformat(pyl, width=100) + "\n", pyl)

# ---- KDL (kdl-py)
import kdl  # noqa: E402

kdl_doc = kdl.parse('title "Sample"\ncount 3\nratio 0.5\nflags #true #false #null\n'
                    'server host="example.invalid" port=8080 {\n  path "/srv"\n}\nitems {\n  - 1\n  - "two"\n}\n')
save("kdl_python.kdl", kdl_doc.print(),
     {"title": "Sample", "count": 3, "ratio": 0.5, "flags": [True, False, None],
      "server": {"host": "example.invalid", "port": 8080, "path": "/srv"}, "items": [1, "two"]})

# ---- S-expressions (sexpdata)
import sexpdata  # noqa: E402

save("sexpdata.sexp", sexpdata.dumps([sexpdata.Symbol("a"), 1, 2.5, "text", [sexpdata.Symbol("b"), [1, 2]]]) + "\n",
     ["a", 1, 2.5, "text", ["b", [1, 2]]])

# ---- HOCON (pyhocon)
from pyhocon import ConfigFactory, HOCONConverter  # noqa: E402

conf = ConfigFactory.parse_string('app { name = "demo", port = 8080, tags = [a, b], nested { level = 2 } }\n'
                                  'copy = ${app.name}\n')
save("pyhocon.conf", HOCONConverter.to_hocon(conf) + "\n",
     {"app": {"name": "demo", "port": 8080, "tags": ["a", "b"], "nested": {"level": 2}}, "copy": "demo"})

# ---- XML (ElementTree)
import xml.etree.ElementTree as ET  # noqa: E402

root = ET.Element("catalog", {"version": "2"})
for i, title in enumerate(["Alpha", "Beta \u00e9"], 1):
    book = ET.SubElement(root, "book", {"id": str(i)})
    ET.SubElement(book, "title").text = title
    ET.SubElement(book, "price").text = f"{i * 10}.50"
ET.SubElement(root, "empty")
ET.indent(root)
save("elementtree.xml", ET.tostring(root, encoding="unicode", xml_declaration=True) + "\n",
     {"catalog": {"@version": "2", "book": [{"@id": "1", "title": "Alpha", "price": "10.50"},
                                             {"@id": "2", "title": "Beta \u00e9", "price": "20.50"}], "empty": None}})

# ---- Java properties (jproperties)
import jproperties  # noqa: E402

props = jproperties.Properties()
props["app.name"] = "demo app"
props["app.greeting"] = "h\u00e9llo = world"
props["path"] = "C:\\data\\x"
props["empty"] = ""
buf = io.BytesIO()
props.store(buf, encoding="latin-1", timestamp=False)
save("jproperties.properties", buf.getvalue(),
     {"app.name": "demo app", "app.greeting": "h\u00e9llo = world", "path": "C:\\data\\x", "empty": ""})
print("written to", out)

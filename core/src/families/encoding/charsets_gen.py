"""Writes charset_tables.inc from Python's codecs (and glibc iconv's IBM-1047).

Not used by the build: run it with Python 3.12+ from this directory after
changing the list of charsets, then commit the regenerated file.

    python3 charsets_gen.py [cp1047.txt]

cp1047.txt holds the 256 code points of IBM-1047 as hex words; it was made
with glibc iconv (`iconv -f IBM1047 -t UTF-32BE` for every byte), because
Python has no cp1047 codec. Without it, IBM-1047 is derived from cp037 by
the six documented swaps.

Single-byte tables are written as arrays (0xFFFF = undefined). The CJK
tables are written as zlib-compressed little-endian uint16 arrays in
hexadecimal chunks, inflated once at run time.
"""
import sys
import zlib

SBCS = [
    # id, python codec
    ("iso-8859-1", "latin_1"), ("iso-8859-2", "iso8859_2"), ("iso-8859-3", "iso8859_3"), ("iso-8859-4", "iso8859_4"),
    ("iso-8859-5", "iso8859_5"), ("iso-8859-6", "iso8859_6"), ("iso-8859-7", "iso8859_7"), ("iso-8859-8", "iso8859_8"),
    ("iso-8859-9", "iso8859_9"), ("iso-8859-10", "iso8859_10"), ("iso-8859-11", "iso8859_11"),
    ("iso-8859-13", "iso8859_13"), ("iso-8859-14", "iso8859_14"), ("iso-8859-15", "iso8859_15"),
    ("iso-8859-16", "iso8859_16"), ("windows-1250", "cp1250"), ("windows-1251", "cp1251"), ("windows-1252", "cp1252"),
    ("windows-1253", "cp1253"), ("windows-1254", "cp1254"), ("windows-1255", "cp1255"), ("windows-1256", "cp1256"),
    ("windows-1257", "cp1257"), ("windows-1258", "cp1258"), ("koi8-r", "koi8_r"), ("koi8-u", "koi8_u"),
    ("macintosh", "mac_roman"), ("ibm437", "cp437"), ("ibm850", "cp850"), ("ibm852", "cp852"), ("ibm866", "cp866"),
    ("ibm037", "cp037"), ("ibm500", "cp500"), ("ibm1047", None),
]


def sbcs_table(codec):
    out = []
    for b in range(256):
        try:
            s = bytes([b]).decode(codec)
            out.append(ord(s) if len(s) == 1 else 0xFFFF)
        except UnicodeDecodeError:
            out.append(0xFFFF)
    return out


def cp1047(path):
    if path:
        return [int(v, 16) for v in open(path).read().split()]
    t = sbcs_table("cp037")
    swaps = {0x5F: 0x5E, 0xAD: 0x5B, 0xB0: 0xAC, 0xBA: 0xDD, 0xBB: 0xA8, 0xBD: 0x5D}
    for k, v in swaps.items():
        t[k] = v
    return t


def dbcs(codec, leads, trails, prefix=b""):
    out = []
    for lead in leads:
        for trail in trails:
            try:
                s = (prefix + bytes([lead, trail])).decode(codec)
                out.append(ord(s) if len(s) == 1 and ord(s) < 0x10000 else 0xFFFF)
            except UnicodeDecodeError:
                out.append(0xFFFF)
    return out


def blob(name, values):
    raw = b"".join(v.to_bytes(2, "little") for v in values)
    text = zlib.compress(raw, 9).hex()
    chunks = [text[i:i + 12000] for i in range(0, len(text), 12000)]
    body = "".join('    "%s",\n' % c for c in chunks)
    return "constexpr std::array<std::string_view, %d> %s_z = {{\n%s}};\nconstexpr std::size_t %s_count = %d;\n" % (
        len(chunks), name, body, name, len(values))


def main():
    out = ["// The output of charsets_gen.py (next to this file), made from the codecs of Python %s;\n"
           "// change the script and run it again rather than editing this file.\n" % sys.version.split()[0],
           "// Single-byte tables map bytes to UTF-16 code units (0xFFFF = undefined).\n\n"]
    out.append("constexpr std::array<SbcsTable, %d> sbcs_tables = {{\n" % len(SBCS))
    for cid, codec in SBCS:
        table = cp1047(sys.argv[1] if len(sys.argv) > 1 else None) if codec is None else sbcs_table(codec)
        out.append('    {"%s", {{\n' % cid)
        for i in range(0, 256, 16):
            out.append("        " + " ".join("0x%04X," % v for v in table[i:i + 16]) + "\n")
        out.append("    }}},\n")
    out.append("}};\n\n")
    full = range(0xA1, 0xFF)
    out.append(blob("jis0208", dbcs("euc_jp", full, full)))           # 94 x 94, row-major
    out.append(blob("jis0212", dbcs("euc_jp", full, full, b"\x8f")))  # 94 x 94
    cp932_leads = list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD))
    out.append(blob("cp932", dbcs("cp932", cp932_leads, range(0x40, 0xFD))))
    gb = dbcs("gb18030", range(0x81, 0xFF), range(0x40, 0xFF))
    out.append(blob("gb18030", gb))                                     # 126 x 191
    gbk = dbcs("gbk", range(0x81, 0xFF), range(0x40, 0xFF))
    patch = [(i, v) for i, (a, v) in enumerate(zip(gb, gbk)) if a != v]
    out.append(blob("gbk_patch", [x for pair in patch for x in pair]))  # index, value pairs
    out.append(blob("big5", dbcs("big5", range(0x81, 0xFF), range(0x40, 0xFF))))
    out.append(blob("ksc5601", dbcs("euc_kr", full, full)))
    # GB18030 four-byte sequences of the BMP: ranges of linear index -> code point.
    ranges = []
    for index in range(0, 39420):
        b1, r = divmod(index, 12600)
        b2, r = divmod(r, 1260)
        b3, b4 = divmod(r, 10)
        seq = bytes([0x81 + b1, 0x30 + b2, 0x81 + b3, 0x30 + b4])
        try:
            cp = ord(seq.decode("gb18030"))
        except UnicodeDecodeError:
            continue
        if ranges and ranges[-1][0] + ranges[-1][2] == index and ranges[-1][1] + ranges[-1][2] == cp:
            ranges[-1][2] += 1
        else:
            ranges.append([index, cp, 1])
    out.append("struct Gb18030Range {\n  std::uint32_t index;\n  std::uint32_t code_point;\n  std::uint32_t length;\n};\n")
    out.append("constexpr std::array<Gb18030Range, %d> gb18030_ranges = {{\n" % len(ranges))
    for i in range(0, len(ranges), 4):
        out.append("    " + " ".join("{%d, 0x%04X, %d}," % tuple(r) for r in ranges[i:i + 4]) + "\n")
    out.append("}};\n")
    open("charset_tables.inc", "w").write("".join(out))


if __name__ == "__main__":
    main()

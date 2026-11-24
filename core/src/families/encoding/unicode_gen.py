"""Writes unicode_tables.inc and html_entities.inc from Python's unicodedata and html.entities.

Not used by the build: run it with Python 3.12+ from this directory after a
Unicode update, then commit the regenerated files.

    python3 unicode_gen.py

unicode_tables.inc holds what normalisation needs: canonical combining
classes (ranges), canonical and compatibility decompositions (one level, as in
UnicodeData.txt) and the primary composites. Hangul syllables are algorithmic
and not listed. The arrays are zlib-compressed little-endian uint32 values in
hexadecimal chunks, inflated once at run time.

html_entities.inc holds the 2231 named character references of HTML5
(html.entities.html5), sorted by name, and the name preferred when writing a
code point (the HTML 4 name where there is one, else the shortest HTML5 name).
"""
import html.entities
import sys
import unicodedata
import zlib


def blob(name, values):
    raw = b"".join(v.to_bytes(4, "little") for v in values)
    text = zlib.compress(raw, 9).hex()
    chunks = [text[i:i + 12000] for i in range(0, len(text), 12000)]
    body = "".join('    "%s",\n' % c for c in chunks)
    return "constexpr std::array<std::string_view, %d> %s_z = {{\n%s}};\nconstexpr std::size_t %s_count = %d;\n" % (
        len(chunks), name, body, name, len(values))


def is_hangul(cp):
    return 0xAC00 <= cp <= 0xD7A3


def unicode_tables():
    ccc = []  # start, end, class
    decomp = []  # flat: cp | (compat << 31), length, then the code points
    for cp in range(0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        ch = chr(cp)
        c = unicodedata.combining(ch)
        if c:
            if ccc and ccc[-1][1] == cp - 1 and ccc[-1][2] == c:
                ccc[-1][1] = cp
            else:
                ccc.append([cp, cp, c])
        d = unicodedata.decomposition(ch)
        if d and not is_hangul(cp):
            compat = d.startswith("<")
            parts = [int(x, 16) for x in d.split() if not x.startswith("<")]
            decomp += [cp | (0x80000000 if compat else 0), len(parts)] + parts
    # Primary composites: pairs whose canonical composition yields one character.
    comp = []
    for cp in range(0x110000):
        if 0xD800 <= cp <= 0xDFFF or is_hangul(cp):
            continue
        d = unicodedata.decomposition(chr(cp))
        if d and not d.startswith("<"):
            parts = [int(x, 16) for x in d.split()]
            if len(parts) == 2 and unicodedata.normalize("NFC", chr(parts[0]) + chr(parts[1])) == chr(cp):
                comp += [parts[0], parts[1], cp]
    flat_ccc = [x for r in ccc for x in r]
    return (blob("ccc", flat_ccc) + blob("decompositions", decomp) + blob("compositions", comp) +
            "constexpr std::string_view unicode_version = \"%s\";\n" % unicodedata.unidata_version)


def entities():
    items = sorted(html.entities.html5.items())
    preferred = {}
    for cp, name in html.entities.codepoint2name.items():
        preferred[cp] = name + ";"
    for name, value in items:
        if name.endswith(";") and len(value) == 1:
            cp = ord(value)
            current = preferred.get(cp)
            if current is None or (len(name), name.lower() != name, name) < (len(current), current.lower() != current, current):
                if cp not in html.entities.codepoint2name:
                    preferred[cp] = name
    out = ["constexpr auto html5_entities = std::to_array<NamedEntity>({\n"]
    for name, value in items:
        cps = [ord(c) for c in value]
        out.append('    {"%s", 0x%04X, 0x%04X},\n' % (name, cps[0], cps[1] if len(cps) > 1 else 0))
    out.append("});\n\n")
    pref = sorted(preferred.items())
    out.append("constexpr auto preferred_names = std::to_array<PreferredName>({\n")
    for cp, name in pref:
        out.append('    {0x%04X, "%s"},\n' % (cp, name))
    out.append("});\n")
    return "".join(out)


header = ("// The output of unicode_gen.py (next to this file), made with Python %s (Unicode %s);\n"
          "// change the script and run it again rather than editing this file.\n" % (sys.version.split()[0], unicodedata.unidata_version))
open("unicode_tables.inc", "w").write(header + unicode_tables())
open("html_entities.inc", "w").write(header + entities())

"""Writes the fixtures of this directory with Python's unicodedata, html and encodings.idna modules.

Run with Python 3.14 (Unicode 16.0) in this directory; not used by the build or the tests.
"""
import hashlib
import html
import random
import unicodedata

# 1. Every code point of planes 0-2 on its own, in all four forms: only digests are stored.
digests = []
for form in ("NFC", "NFD", "NFKC", "NFKD"):
    h = hashlib.sha256()
    for cp in range(0x30000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        h.update(unicodedata.normalize(form, chr(cp)).encode("utf-8"))
        h.update(b"\n")
    digests.append("%s %s" % (form, h.hexdigest()))
open("single_code_points.sha256", "w").write("\n".join(digests) + "\n")

# 2. Sequences that exercise reordering and composition (deterministic pseudo-random choice).
rng = random.Random(20240601)
bases = [0x41, 0x61, 0x45, 0x65, 0x4F, 0x6F, 0x55, 0x75, 0x3B1, 0x3C9, 0x438, 0x1100, 0xAC00, 0xAC01, 0x1E0A, 0xD6, 0x212B,
         0x1161, 0x11A8, 0x0B47, 0x0915, 0x3099, 0x304B, 0x30AB, 0xFB01, 0x2126, 0x1F00, 0x0627, 0x05D0]
marks = [0x300, 0x301, 0x302, 0x303, 0x308, 0x323, 0x327, 0x328, 0x31B, 0x345, 0x5B4, 0x93C, 0x94D, 0x3099, 0x309A,
         0x1161, 0x11A8, 0x0B3E, 0x0B57, 0x0F71, 0x0F72, 0x0F80, 0x1D165, 0x1D16E, 0x20D7, 0x302A]
lines = []
for _ in range(400):
    seq = [rng.choice(bases)] + [rng.choice(marks) for _ in range(rng.randint(0, 4))]
    if rng.random() < 0.3:
        seq += [rng.choice(bases)] + [rng.choice(marks) for _ in range(rng.randint(0, 3))]
    s = "".join(chr(c) for c in seq)
    cols = [s] + [unicodedata.normalize(f, s) for f in ("NFC", "NFD", "NFKC", "NFKD")]
    lines.append(";".join(" ".join("%04X" % ord(c) for c in col) for col in cols))
open("sequences.txt", "w").write("\n".join(lines) + "\n")

# 3. HTML character references as html.unescape decodes them.
cases = ["&amp;", "&amp", "&ampx", "&notit;", "&notin;", "&not", "&copy2024", "&NotEqualTilde;", "&nbsp;x", "&lt;b&gt;",
         "&#x1F600;", "&#128512;", "&#38;", "&#x26;", "&Aacute;&aacute", "&unknown;", "& amp;", "&#xZZ;", "&;",
         "&ThickSpace;", "&fjlig;", "&bne;", "&acE;", "&Uuml;ber", "&quot;q&quot;", "&lt&gt", "&LT;&GT;", "a&b&c",
         "&eacute;t&eacute;", "&rarr;&larr;&hArr;", "&frac12;&frac34;", "&euro;100", "&sup2;&sup3;", "&hellip;"]
with open("entities.txt", "w") as f:
    for c in cases:
        f.write("%s\t%s\n" % (c, " ".join("%04X" % ord(ch) for ch in html.unescape(c))))

# 4. IDNA 2003 ToASCII from Python's encodings.idna for simple names.
names = ["bücher.example", "Bücher.Example", "例え.テスト", "παράδειγμα.δοκιμή", "пример.испытание", "ドメイン名例.jp",
         "münchen.de", "ＢÜＣＨＥＲ。example", "faß.de", "straße.example"]
with open("idna.txt", "w") as f:
    for n in names:
        f.write("%s\t%s\n" % (n, n.encode("idna").decode()))
print("ok")

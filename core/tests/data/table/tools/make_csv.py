"""Delimited-text fixtures written with Python's csv module (standard library)."""
import csv
import io
import os
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
P = lambda name: os.path.join(out, name)

ROWS = [
    ["name", "qty", "price", "when", "ok", "note"],
    ["Tea", "3", "1.50", "2024-05-01", "true", "plain"],
    ["Milk, oat", "12", "0.99", "2024-05-02T10:30:00", "false", 'says "hi"'],
    ["Grüße 世界", "-7", "1e-05", "", "TRUE", "line one\nline two"],
    ["007", "0", "", "1999-12-31", "", ""],
    ["  padded  ", "12345678901234567890", "3.14159", "2024-02-29 23:59:59", "FALSE", "tab\there"],
]

with open(P("excel_crlf.csv"), "w", newline="", encoding="utf-8") as f:
    csv.writer(f).writerows(ROWS)
with open(P("unix_all_quoted.csv"), "w", newline="", encoding="utf-8") as f:
    csv.writer(f, dialect="unix").writerows(ROWS)
with open(P("excel_tab.tsv"), "w", newline="", encoding="utf-8") as f:
    csv.writer(f, dialect="excel-tab").writerows(ROWS)
with open(P("pipe.psv"), "w", newline="", encoding="utf-8") as f:
    csv.writer(f, delimiter="|", lineterminator="\n").writerows(ROWS)

# Semicolons and decimal commas, UTF-8 with a byte order mark (Excel in many European locales).
EURO = [["Artikel", "Menge", "Preis"], ["Käse", "2", "3,50"], ["Brot; dunkel", "1", "2,25"], ["Wein", "-1", "12,00"]]
with open(P("semicolon_bom.csv"), "w", newline="", encoding="utf-8-sig") as f:
    csv.writer(f, delimiter=";").writerows(EURO)
# Excel's "sep=" hint line.
buffer = io.StringIO()
csv.writer(buffer, delimiter=";", lineterminator="\r\n").writerows(EURO)
with open(P("sep_hint.csv"), "w", newline="", encoding="utf-8") as f:
    f.write("sep=;\r\n" + buffer.getvalue())
# Excel "Unicode text": tab separated UTF-16LE with a BOM.
with open(P("utf16le_bom.tsv"), "w", newline="", encoding="utf-16") as f:
    csv.writer(f, dialect="excel-tab").writerows(ROWS)
with open(P("utf16be.csv"), "w", newline="", encoding="utf-16-be") as f:
    csv.writer(f).writerows(ROWS[:3])
# Legacy single-byte encoding.
with open(P("cp1252.csv"), "w", newline="", encoding="cp1252") as f:
    csv.writer(f).writerows([["word", "price"], ["café", "4"], ["naïve", "5"], ["€uro", "6"]])
# MySQL-style backslash escapes instead of doubled quotes.
with open(P("backslash_escape.csv"), "w", newline="", encoding="utf-8") as f:
    csv.writer(f, escapechar="\\", doublequote=False, quoting=csv.QUOTE_NONNUMERIC).writerows(
        [["id", "text"], [1, 'he said "yes"'], [2, "back\\slash"]])
# Ragged rows and no final line break.
with open(P("ragged.csv"), "w", newline="", encoding="utf-8") as f:
    csv.writer(f, lineterminator="\n").writerows([["a", "b", "c"], ["1"], ["2", "3", "4", "5"], [], ["6", "7"]])
    f.seek(f.tell() - 1)
    f.truncate()
# Fixed-width report made with str.ljust / str.rjust.
lines = [
    "Item".ljust(12) + "Qty".rjust(6) + "  " + "Price".rjust(8) + "  " + "Date",
    "Tea".ljust(12) + "3".rjust(6) + "  " + "1.50".rjust(8) + "  " + "2024-05-01",
    "Milk oat".ljust(12) + "12".rjust(6) + "  " + "0.99".rjust(8) + "  " + "2024-05-02",
    "Grüße".ljust(12) + "-7".rjust(6) + "  " + "100.00".rjust(8) + "  " + "1999-12-31",
]
with open(P("report.fwf"), "w", newline="\n", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")

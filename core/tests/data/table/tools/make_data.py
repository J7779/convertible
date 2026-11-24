"""Columnar, database and text fixtures for the table family (run inside the fixture container)."""
import datetime as dt
import decimal
import os
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
P = lambda name: os.path.join(out, name)

# ---------------------------------------------------------------- Parquet (pyarrow)
import pyarrow as pa
import pyarrow.parquet as pq

table = pa.table({
    "id": pa.array([1, 2, 3, 4, 5], pa.int64()),
    "small": pa.array([1, -2, None, 4, 127], pa.int8()),
    "u32": pa.array([0, 1, 4000000000, None, 7], pa.uint32()),
    "ratio": pa.array([0.5, -1.25, None, 3.0e10, 1e-7], pa.float64()),
    "f32": pa.array([1.5, None, -2.25, 0.0, 100.0], pa.float32()),
    "name": pa.array(["alpha", "Grüße", None, "世界", ""], pa.string()),
    "flag": pa.array([True, False, None, True, False], pa.bool_()),
    "day": pa.array([dt.date(2024, 5, 1), dt.date(1970, 1, 1), None, dt.date(1999, 12, 31), dt.date(2100, 2, 28)], pa.date32()),
    "ts_ms": pa.array([dt.datetime(2024, 5, 1, 12, 30), None, dt.datetime(2000, 1, 1, 0, 0, 1), dt.datetime(1960, 2, 29, 6), dt.datetime(2024, 2, 29, 23, 59, 59)], pa.timestamp("ms")),
    "ts_us": pa.array([dt.datetime(2024, 5, 1, 12, 30, 0, 250000), None, None, None, dt.datetime(1970, 1, 1)], pa.timestamp("us")),
    "tod": pa.array([dt.time(12, 30), dt.time(0, 0, 1), None, dt.time(23, 59, 59), dt.time(6)], pa.time64("us")),
    "price": pa.array([decimal.Decimal("12.34"), decimal.Decimal("-0.01"), None, decimal.Decimal("99999.99"), decimal.Decimal("0.00")], pa.decimal128(9, 2)),
    "big_dec": pa.array([decimal.Decimal("123456789012345678.901"), None, decimal.Decimal("-1.000"), decimal.Decimal("0.001"), decimal.Decimal("5")], pa.decimal128(25, 3)),
})
pq.write_table(table, P("types_snappy.parquet"), compression="snappy")
pq.write_table(table, P("types_gzip.parquet"), compression="gzip")
pq.write_table(table, P("types_none_v2.parquet"), compression="none", data_page_version="2.0")
pq.write_table(table, P("types_plain_nodict.parquet"), compression="none", use_dictionary=False)
pq.write_table(table, P("types_delta.parquet"), compression="none", use_dictionary=False,
               column_encoding={"id": "DELTA_BINARY_PACKED", "small": "DELTA_BINARY_PACKED", "name": "DELTA_BYTE_ARRAY",
                                "ratio": "BYTE_STREAM_SPLIT", "f32": "BYTE_STREAM_SPLIT", "u32": "DELTA_BINARY_PACKED"})
pq.write_table(table.select(["id", "name"]), P("delta_length.parquet"), compression="none", use_dictionary=False,
               column_encoding={"name": "DELTA_LENGTH_BYTE_ARRAY", "id": "PLAIN"})
pq.write_table(table.select(["id", "ts_ms"]), P("int96.parquet"), compression="none", use_deprecated_int96_timestamps=True)
# Several row groups and a repeated dictionary.
many = pa.table({"n": pa.array(range(3000), pa.int64()), "colour": pa.array([["red", "green", "blue"][i % 3] for i in range(3000)])})
pq.write_table(many, P("rowgroups.parquet"), compression="snappy", row_group_size=1000)
# Nested: struct and list columns.
nested = pa.table({
    "id": pa.array([1, 2, 3]),
    "point": pa.array([{"x": 1.0, "y": 2.0}, None, {"x": -1.5, "y": 0.0}], pa.struct([("x", pa.float64()), ("y", pa.float64())])),
    "tags": pa.array([["a", "b"], [], None], pa.list_(pa.string())),
})
pq.write_table(nested, P("nested.parquet"), compression="none")

# ---------------------------------------------------------------- SQLite (Python's sqlite3)
import sqlite3
for name in ("sample.sqlite", "utf16_small_pages.sqlite"):
    path = P(name)
    if os.path.exists(path):
        os.remove(path)
c = sqlite3.connect(P("sample.sqlite"))
c.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL, height REAL, born DATE, active BOOLEAN, photo BLOB)")
rows = [(1, "Ada", 1.65, "1990-04-01", 1, b"\x00\x01\xff"), (2, "Grüße 世界", None, None, 0, None), (3, "O'Brien \"quoted\"", -0.5, "2024-02-29", 1, b"")]
c.executemany("INSERT INTO people VALUES (?, ?, ?, ?, ?, ?)", rows)
c.execute('CREATE TABLE "odd name" ("col a" INTEGER, [col b] TEXT, `col c` NUMERIC, gen INTEGER GENERATED ALWAYS AS ("col a" * 2) VIRTUAL)')
c.executemany('INSERT INTO "odd name" ("col a", [col b], `col c`) VALUES (?, ?, ?)', [(i, "v%d" % i, i * 1.5) for i in range(1, 601)])
c.execute("CREATE TABLE pairs (k TEXT, n INTEGER, v TEXT, PRIMARY KEY (n, k)) WITHOUT ROWID")
c.executemany("INSERT INTO pairs VALUES (?, ?, ?)", [("k%03d" % i, i % 5, "x" * (i % 40)) for i in range(300)])
c.execute("CREATE TABLE long (id INTEGER PRIMARY KEY, body TEXT)")
c.execute("INSERT INTO long (body) VALUES (?)", ("lorem ipsum " * 900,))
c.execute("INSERT INTO long (body) VALUES (?)", ("short",))
c.execute("CREATE INDEX people_name ON people(name)")
c.execute("CREATE VIEW tall AS SELECT name FROM people WHERE height > 1")
c.execute("ALTER TABLE people ADD COLUMN note TEXT")
c.execute("UPDATE people SET note = 'added later' WHERE id = 3")
c.commit()
c.execute("VACUUM")
c.close()
c = sqlite3.connect(P("utf16_small_pages.sqlite"))
c.execute("PRAGMA page_size = 512")
c.execute("PRAGMA encoding = 'UTF-16le'")
c.execute("CREATE TABLE words (w TEXT, n INTEGER)")
c.executemany("INSERT INTO words VALUES (?, ?)", [("wörd %d ✓" % i, i * i) for i in range(400)])
c.commit()
c.execute("VACUUM")
c.close()

# ---------------------------------------------------------------- Excel 97 via xlwt (independent BIFF8 writer)
import xlwt
wb = xlwt.Workbook(encoding="utf-8")
ws = wb.add_sheet("Data")
date_style = xlwt.easyxf(num_format_str="YYYY-MM-DD")
dt_style = xlwt.easyxf(num_format_str="YYYY-MM-DD HH:MM:SS")
for c_, title in enumerate(["text", "number", "date", "when", "flag"]):
    ws.write(0, c_, title)
ws.write(1, 0, "hello")
ws.write(1, 1, 42)
ws.write(1, 2, dt.date(2024, 5, 1), date_style)
ws.write(1, 3, dt.datetime(2024, 5, 1, 12, 30), dt_style)
ws.write(1, 4, True)
ws.write(2, 0, "Grüße 世界 😀")
ws.write(2, 1, -0.125)
ws.write(2, 2, dt.date(1999, 12, 31), date_style)
ws.write(2, 4, False)
ws.write(3, 0, "line one\nline two")
ws.write(3, 1, 123456789.5)
ws.write_merge(5, 5, 0, 2, "merged across")
# Many unique strings so that the SST spans CONTINUE records, including long
# strings and wide (UTF-16) strings that cross record boundaries.
big = wb.add_sheet("Strings")
for i in range(600):
    big.write(i, 0, "string number %05d" % i)
    big.write(i, 1, ("ü%d" % i) * (3 if i % 2 else 1))
big.write(600, 0, "L" * 9000)
big.write(601, 0, "Ж" * 5000)
hidden = wb.add_sheet("Hidden")
hidden.write(0, 0, "secret")
hidden.visibility = 1
wb.save(P("xlwt_biff8.xls"))

# ---------------------------------------------------------------- dBase via the dbf package
import dbf
path = P("dbase3.dbf")
t = dbf.Table(path, "name C(20); qty N(5,0); price N(8,2); born D; ok L", dbf_type="db3", codepage="cp1252")
t.open(dbf.READ_WRITE)
for rec in [("Tea", 3, 1.5, dt.date(2024, 5, 1), True), ("Café", -12, 0.99, dt.date(1999, 12, 31), False), ("", 0, 0, None, None)]:
    t.append(rec)
t.append(("deleted", 1, 1, None, True))
dbf.delete(t[-1])
t.close()
path = P("foxpro.dbf")
t = dbf.Table(path, "name C(10); count I; amount Y; stamp T; ratio B; d D", dbf_type="vfp", codepage="cp1252")
t.open(dbf.READ_WRITE)
t.append(("alpha", 7, decimal.Decimal("12.3456"), dt.datetime(2024, 5, 1, 12, 30, 15), 2.5, dt.date(2024, 5, 1)))
t.append(("beta", -1, decimal.Decimal("-0.5"), dt.datetime(2000, 1, 1, 0, 0, 0), -1e-3, dt.date(1970, 1, 1)))
t.close()

# ---------------------------------------------------------------- Statistics packages
import pandas as pd
frame = pd.DataFrame({
    "id": pd.Series([1, 2, 3], dtype="int32"),
    "score": [1.5, -2.25, None],
    "label": ["alpha", "beta", "gamma"],
    "when": pd.to_datetime(["2024-05-01 12:30:00", "1999-12-31 00:00:00", "1960-02-29 06:00:00"]),
})
frame.to_stata(P("sample.dta"), write_index=False, version=118, time_stamp=dt.datetime(2000, 1, 1))
frame.to_stata(P("sample_v114.dta"), write_index=False, version=114, time_stamp=dt.datetime(2000, 1, 1))
import pyreadstat
plain = frame.drop(columns=["when"])
pyreadstat.write_sav(frame, P("sample.sav"))
pyreadstat.write_xport(plain, P("sample.xpt"), table_name="SAMPLE", file_format_version=5)

# ---------------------------------------------------------------- Text tables (tabulate)
from tabulate import tabulate
header = ["Item", "Qty", "Price", "Note"]
data = [["Tea", 3, 1.5, "green"], ["Grüße", -12, 0.99, "世界"], ["pipe|char", 0, 1e-05, ""]]
for fmt, ext in [("github", "md"), ("grid", "rst"), ("rst", "simple.rst"), ("orgtbl", "org"), ("jira", "jira"),
                 ("mediawiki", "wiki"), ("latex", "tex"), ("latex_booktabs", "booktabs.tex"), ("html", "html"),
                 ("asciidoc", "adoc"), ("psql", "psql.txt"), ("simple_grid", "box.txt")]:
    text = tabulate([[x.replace("|", "\\|") if fmt in ("github",) and isinstance(x, str) else x for x in row] for row in data],
                    headers=header, tablefmt=fmt)
    with open(P("tabulate." + ext), "w", encoding="utf-8") as f:
        f.write(text + "\n")

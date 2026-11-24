"""Open Convertible's output with independent readers (run inside the fixture container)."""
import os
import subprocess
import sys

I = sys.argv[1]
os.chdir(I)
results = []


def check(name, fn):
    try:
        detail = fn()
        results.append(("OK  ", name, detail))
    except Exception as e:  # noqa: BLE001
        results.append(("FAIL", name, repr(e)[:300]))


import openpyxl
def xlsx(path):
    def run():
        wb = openpyxl.load_workbook(path)
        ws = wb["Types"] if "Types" in wb.sheetnames else wb.worksheets[0]
        rows = list(ws.iter_rows(values_only=True))
        return f"sheets={wb.sheetnames} states={[s.sheet_state for s in wb.worksheets]} rows={len(rows)} r1={rows[1] if len(rows) > 1 else None} merged={[str(m) for s in wb.worksheets for m in s.merged_cells.ranges]} freeze={ws.freeze_panes}"
    return run
for p in ("ours.xlsx", "ours.xlsm", "ours.xltx", "big.xlsx", "strings.xlsx"):
    check("openpyxl " + p, xlsx(p))

import xlrd
def xls(path):
    def run():
        book = xlrd.open_workbook(path, formatting_info=True)
        s = book.sheet_by_index(0)
        names = book.sheet_names()
        row1 = [s.cell_value(1, c) for c in range(s.ncols)] if s.nrows > 1 else None
        dates = [xlrd.xldate_as_datetime(s.cell_value(1, c), book.datemode).isoformat() for c in range(s.ncols) if s.cell_type(1, c) == xlrd.XL_CELL_DATE] if s.nrows > 1 else []
        return f"sheets={names} vis={[x.visibility for x in book.sheets()]} rows={s.nrows} r1={row1} dates={dates} merged={s.merged_cells}"
    return run
for p in ("ours.xls", "big.xls", "strings.xls"):
    check("xlrd " + p, xls(p))
def strings_check():
    book = xlrd.open_workbook("strings.xls")
    s = book.sheet_by_name("Strings")
    assert s.cell_value(1500, 0) == "L" * 9000
    assert s.cell_value(1501, 0) == "Ж" * 5000
    assert s.cell_value(777, 0) == "string number 00777"
    return "long and wide SST strings intact"
check("xlrd strings.xls SST", strings_check)

import pyarrow.parquet as pq
for p in ("ours_snappy.parquet", "ours_gzip.parquet", "ours_none.parquet"):
    def run(p=p):
        t = pq.read_table(p)
        return f"schema={[(f.name, str(f.type)) for f in t.schema]} rows={t.num_rows} first={t.slice(0, 1).to_pylist()}"
    check("pyarrow " + p, run)

import sqlite3
def sq():
    c = sqlite3.connect("ours.sqlite")
    return f"integrity={c.execute('pragma integrity_check').fetchone()} tables={[r[0] for r in c.execute('select name from sqlite_schema')]} types_row={c.execute('select * from Types limit 1').fetchone()}"
check("python sqlite3 ours.sqlite", sq)
def sqlscript():
    c = sqlite3.connect(":memory:")
    c.executescript(open("ours_sqlite.sql", encoding="utf-8").read())
    return f"rows={c.execute('select count(*) from Types').fetchone()} first={c.execute('select * from Types limit 1').fetchone()}"
check("python sqlite3 executes ours_sqlite.sql", sqlscript)

import dbf
for p in ("ours.dbf", "ours_types.dbf"):
    def run(p=p):
        t = dbf.Table(p)
        t.open()
        recs = [tuple(r) for r in t]
        t.close()
        return f"fields={t.field_names} n={len(recs)} first={recs[0] if recs else None}"
    check("dbf " + p, run)

# LibreOffice opens each and re-exports CSV of the first sheet.
os.makedirs("lo", exist_ok=True)
for p in ("ours.xlsx", "ours.xls", "ours.ods", "ours.fods", "ours.spreadsheetml", "ours.html", "ours.dbf", "ours.csv"):
    src = p
    if p.endswith(".spreadsheetml"):
        src = "ours_2003.xml"
        os.replace(p, src) if os.path.exists(p) else None
    def run(src=src):
        out = subprocess.run(["soffice", "--headless", "--convert-to", "csv:Text - txt - csv (StarCalc):44,34,76,1,,0,false,true,false,false,false,-1", "--outdir", "lo", src],
                             capture_output=True, text=True, timeout=120)
        base = os.path.splitext(src)[0]
        produced = sorted(f for f in os.listdir("lo") if f.startswith(base))
        if not produced:
            raise RuntimeError("no output: " + out.stdout + out.stderr)
        first = open(os.path.join("lo", produced[0]), encoding="utf-8", errors="replace").read().splitlines()[:3]
        return f"files={produced} head={first}"
    check("LibreOffice " + src, run)

for status, name, detail in results:
    print(status, name, "::", detail)

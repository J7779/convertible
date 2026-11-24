"""Synthetic workbooks for the table family fixtures (run inside the fixture container)."""
import datetime as dt
import os
import sys

import openpyxl
from openpyxl.utils.datetime import CALENDAR_MAC_1904

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

TYPES = [
    ["Label", "Integer", "Float", "Big", "Bool", "Date", "DateTime", "Time", "Text"],
    ["plain", 42, 3.25, 12345678901, True, dt.date(2024, 5, 1), dt.datetime(2024, 5, 1, 12, 30, 0), dt.time(12, 30), "hello"],
    ["negative", -7, -0.125, -98765432109, False, dt.date(1999, 12, 31), dt.datetime(2000, 1, 1, 0, 0, 1), dt.time(0, 0, 1), "Grüße, 世界"],
    ["quotes", 0, 1e-05, 1e20, True, dt.date(1970, 1, 1), dt.datetime(1960, 2, 29, 6, 0, 0), None, 'say "hi", it\'s'],
    ["multiline", 1000000, 2.5, None, None, dt.date(2100, 12, 31), None, dt.time(23, 59, 59), "line one\nline two"],
    ["emoji", None, 0.1, 123, False, None, dt.datetime(2024, 2, 29, 23, 59, 59), dt.time(6, 0), "😀 smile"],
]


def fill_types(ws):
    for r, row in enumerate(TYPES, start=1):
        for c, value in enumerate(row, start=1):
            if value is None:
                continue
            cell = ws.cell(row=r, column=c, value=value)
            if isinstance(value, dt.datetime):
                cell.number_format = "yyyy-mm-dd hh:mm:ss"
            elif isinstance(value, dt.date):
                cell.number_format = "yyyy-mm-dd"
            elif isinstance(value, dt.time):
                cell.number_format = "hh:mm:ss"


def base_workbook(path):
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Types"
    fill_types(ws)
    f = wb.create_sheet("Formulas")
    f["A1"] = 1
    f["A2"] = 2
    f["A3"] = 3
    f["A4"] = "=SUM(A1:A3)"
    f["B1"] = "=A1*2"
    f["B2"] = '=CONCATENATE("a","b")'
    f["B3"] = "=A3>2"
    f["B4"] = '=IF(A1>0,"pos","neg")'
    f["C1"] = "=ROUND(PI(),2)"
    m = wb.create_sheet("Merged")
    m["A1"] = "Title"
    m.merge_cells("A1:C1")
    m["A2"], m["B2"], m["C2"] = "a", "b", "c"
    m["A3"], m["B3"], m["C3"] = 1, 2, 3
    m["B4"] = "tall"
    m.merge_cells("B4:B5")
    h = wb.create_sheet("Hidden")
    h["A1"] = "secret"
    h["A2"] = 99
    h.sheet_state = "hidden"
    wb.properties.creator = None
    wb.properties.lastModifiedBy = None
    fixed = dt.datetime(2000, 1, 1)
    wb.properties.created = fixed
    wb.properties.modified = fixed
    wb.save(path)


base_workbook(os.path.join(out, "sample_openpyxl.xlsx"))

# The 1904 date system.
wb = openpyxl.Workbook()
wb.epoch = CALENDAR_MAC_1904
ws = wb.active
ws.title = "Mac"
ws["A1"] = "when"
ws["A2"] = dt.date(2024, 5, 1)
ws["A2"].number_format = "yyyy-mm-dd"
ws["A3"] = dt.datetime(1904, 1, 2, 6, 0)
ws["A3"].number_format = "yyyy-mm-dd hh:mm"
ws["B1"] = "n"
ws["B2"] = 1.5
wb.properties.creator = None
wb.properties.created = dt.datetime(2000, 1, 1)
wb.properties.modified = dt.datetime(2000, 1, 1)
wb.save(os.path.join(out, "dates_1904_openpyxl.xlsx"))

# A 10,000-row sheet (kept small by repetitive content and compression).
wb = openpyxl.Workbook(write_only=True)
ws = wb.create_sheet("Big")
ws.append(["value"])
for i in range(1, 10001):
    ws.append([7])
wb.properties.creator = None
wb.properties.created = dt.datetime(2000, 1, 1)
wb.properties.modified = dt.datetime(2000, 1, 1)
wb.save(os.path.join(out, "big_10k_openpyxl.xlsx"))

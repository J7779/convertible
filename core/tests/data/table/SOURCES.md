# Sources: table

Fixtures of the table family tests (`core/tests/families/table/`). Every file
holds synthetic content only and was written by software other than
Convertible. The generator scripts are in `tools/`; commands below are run
in this directory unless noted.

## The sample workbook

`tools/make_workbooks.py` builds it with openpyxl; most spreadsheet fixtures
are that workbook saved by another application. It has four sheets
(`table_test_support.hpp` spells out every cell the tests expect):

- **Types** (row 1 = titles `Label Integer Float Big Bool Date DateTime Time Text`), 5 data rows:
  `plain 42 3.25 12345678901 TRUE 2024-05-01 2024-05-01T12:30:00 12:30:00 hello`,
  `negative -7 -0.125 -98765432109 FALSE 1999-12-31 2000-01-01T00:00:01 00:00:01 "Grüße, 世界"`,
  `quotes 0 1e-05 1e+20 TRUE 1970-01-01 1960-02-29T06:00:00 (empty) say "hi", it's`,
  `multiline 1000000 2.5 (empty) (empty) 2100-12-31 (empty) 23:59:59 "line one⏎line two"`,
  `emoji (empty) 0.1 123 FALSE (empty) 2024-02-29T23:59:59 06:00:00 "😀 smile"`;
  dates and times carry number formats `yyyy-mm-dd`, `yyyy-mm-dd hh:mm:ss`, `hh:mm:ss`.
- **Formulas**: A1..A3 = 1, 2, 3; A4 `=SUM(A1:A3)` (6); B1 `=A1*2` (2); B2 `=CONCATENATE("a","b")` ("ab");
  B3 `=A3>2` (TRUE); B4 `=IF(A1>0,"pos","neg")` ("pos"); C1 `=ROUND(PI(),2)` (3.14).
- **Merged**: A1 "Title" merged over A1:C1; row 2 `a b c`; row 3 `1 2 3`; B4 "tall" merged over B4:B5.
- **Hidden**: a hidden sheet with A1 "secret", A2 99.

Tools (in a throwaway Ubuntu 24.04 container built from `tools/Dockerfile`):
LibreOffice 24.2.7.2 (`soffice`), Gnumeric 1.12.56 (`ssconvert`), Python 3.12.3 with
openpyxl 3.1.5, xlwt 1.3.0, pyarrow 26.0.1, pandas 3.1.1, pyreadstat 1.3.8, dbf 0.99.11,
tabulate 0.10.0 and the sqlite3 module (SQLite 3.45.1). The CSV fixtures were written
with Python 3.14's `csv` module on the host. `tools/run_all.sh` runs the whole
container part; it was run under a numeric user id without an account
(`docker run --user 12345 ...`) so that no account name ends up in a file.

| File | Contents | Tool | Command |
| --- | --- | --- | --- |
| `openpyxl_sample.xlsx` | the sample workbook; formulas have no cached values | openpyxl 3.1.5 | `python tools/make_workbooks.py out` (`out/sample_openpyxl.xlsx`) |
| `openpyxl_dates_1904.xlsx` | sheet "Mac" in the 1904 date system: A1 `when`, A2 2024-05-01, A3 1904-01-02 06:00, B1 `n`, B2 1.5 | openpyxl 3.1.5 | same script (`dates_1904_openpyxl.xlsx`) |
| `openpyxl_big_10k.xlsx` | sheet "Big": title `value` then 10,000 rows of the number 7 (write-only mode) | openpyxl 3.1.5 | same script (`big_10k_openpyxl.xlsx`) |
| `lo_sample.xlsx` | the sample workbook with cached formula results | LibreOffice 24.2 | `soffice --headless --convert-to xlsx:"Calc MS Excel 2007 XML" --outdir lo sample_openpyxl.xlsx` |
| `lo_sample.xls` | the same as BIFF8 | LibreOffice 24.2 | `soffice --headless --convert-to xls:"MS Excel 97" --outdir lo sample_openpyxl.xlsx` |
| `lo_sample.ods` | the same as ODS | LibreOffice 24.2 | `soffice --headless --convert-to ods --outdir lo sample_openpyxl.xlsx` |
| `lo_sample.fods` | the same as flat ODS | LibreOffice 24.2 | `soffice --headless --convert-to fods --outdir lo sample_openpyxl.xlsx` |
| `lo_sample.html` | the visible sheets as HTML tables under "Sheet n: name" headings | LibreOffice 24.2 | `soffice --headless --convert-to html:"HTML (StarCalc)" --outdir lo sample_openpyxl.xlsx` |
| `lo_sample.slk` | sheet Types as SYLK (LibreOffice writes dates as bare serial numbers) | LibreOffice 24.2 | `soffice --headless --convert-to slk:"SYLK" --outdir lo sample_openpyxl.xlsx` |
| `lo_sample.dif` | sheet Types as DIF (formatted values in numeric items) | LibreOffice 24.2 | `soffice --headless --convert-to dif:"DIF" --outdir lo sample_openpyxl.xlsx` |
| `lo_sample_2003.xml` | the sample workbook as SpreadsheetML 2003 (LibreOffice writes its formulas in OpenFormula notation and drops number formats) | LibreOffice 24.2 XSLT export | `soffice --headless --convert-to "xml:MS Excel 2003 XML" --outdir lo sample_compact.ods`, where `sample_compact.ods` is the sample workbook as an ODS without the 16,384 repeated column definitions that make LibreOffice's XSLT recurse too deeply (it was made by Convertible's ODS writer from `lo_sample.xlsx`; the XML itself is LibreOffice's) |
| `gnumeric_sample.gnumeric` | the sample workbook; Gnumeric stores formulas without results | ssconvert 1.12.56 | `ssconvert sample_openpyxl.xlsx gnm.gnumeric` |
| `gnumeric_biff5.xls` | the sample workbook as Excel 5/95 (BIFF5, Windows-1252: 世界 and 😀 became `?`); its "last saved by" field reads `Unknown` | ssconvert 1.12.56 | `ssconvert -T Gnumeric_Excel:excel_biff7 sample_openpyxl.xlsx gnm_biff5.xls` |
| `gnumeric_sample.slk` | sheet Types as SYLK with number formats (Windows-1252) | ssconvert 1.12.56 | `ssconvert -T Gnumeric_sylk:sylk sample_openpyxl.xlsx gnm.slk` |
| `gnumeric_sample.dif` | sheet Types as DIF | ssconvert 1.12.56 | `ssconvert -T Gnumeric_dif:dif sample_openpyxl.xlsx gnm.dif` |
| `xlwt_biff8.xls` | sheets "Data" (text, numbers, dates, a date-time, booleans, a line break, A6:C6 merged), "Strings" (600 rows `string number 00000`.. plus `ü<n>` repeated, a 9,000-character and a 5,000 × `Ж` string: the SST spans CONTINUE records) and a hidden "Hidden" | xlwt 1.3.0 | `python tools/make_data.py out` (`xlwt_biff8.xls`) |
| `pyarrow_types_snappy.parquet` | 5 rows × 13 columns: int64, int8, uint32, double, float, string, bool, date32, timestamp[ms], timestamp[us], time64[us], decimal(9,2), decimal(25,3) with nulls (values in `data_formats_test.cpp`) | pyarrow 26.0.1 | `make_data.py`: `pq.write_table(table, ..., compression="snappy")` |
| `pyarrow_types_gzip.parquet` | the same, gzip pages | pyarrow 26.0.1 | `compression="gzip"` |
| `pyarrow_types_none_v2.parquet` | the same, uncompressed data pages v2 | pyarrow 26.0.1 | `compression="none", data_page_version="2.0"` |
| `pyarrow_types_plain_nodict.parquet` | the same, PLAIN encoding without dictionaries | pyarrow 26.0.1 | `use_dictionary=False` |
| `pyarrow_types_delta.parquet` | the same with DELTA_BINARY_PACKED, DELTA_BYTE_ARRAY and BYTE_STREAM_SPLIT columns | pyarrow 26.0.1 | `column_encoding={...}` (see script) |
| `pyarrow_delta_length.parquet` | columns id, name with DELTA_LENGTH_BYTE_ARRAY strings | pyarrow 26.0.1 | see script |
| `pyarrow_int96.parquet` | columns id, ts_ms as deprecated INT96 timestamps | pyarrow 26.0.1 | `use_deprecated_int96_timestamps=True` |
| `pyarrow_rowgroups.parquet` | 3,000 rows `n` = 0..2999, `colour` = red/green/blue in three row groups | pyarrow 26.0.1 | `row_group_size=1000` |
| `pyarrow_nested.parquet` | id, struct `point{x,y}` and list<string> `tags` with null and empty values | pyarrow 26.0.1 | see script |
| `pyarrow_types_lz4.feather` | 3 rows × 15 columns: int64, int8, uint16, double, float, string, bool, date32, timestamp[ms], time64[us], decimal(9,2), a dictionary-encoded string, struct `point{x,y}`, list<string> `tags`, with nulls (values in `arrow_test.cpp`); Feather 2 with LZ4-compressed buffers | pyarrow 26.0.1 | `python tools/make_arrow.py out`: `feather.write_feather(table, ...)` |
| `pyarrow_types_plain.feather` | the same, uncompressed | pyarrow 26.0.1 | `write_feather(..., compression="uncompressed")` |
| `pyarrow_types.arrows` | the same as an IPC stream of two record batches | pyarrow 26.0.1 | `ipc.new_stream(...)`, `write_table(table, max_chunksize=2)` |
| `python_sample.sqlite` | tables `people` (INTEGER PRIMARY KEY, REAL, DATE, BOOLEAN, BLOB, a column added by ALTER TABLE), `odd name` (quoted names, NUMERIC, a virtual generated column, 600 rows: interior pages), `pairs` (WITHOUT ROWID, composite key), `long` (10,800-character text: overflow pages), an index and a view | Python sqlite3 (SQLite 3.45.1) | `make_data.py` |
| `python_utf16_small_pages.sqlite` | table `words` (400 rows `wörd <n> ✓`, n²) with `PRAGMA page_size=512` and `PRAGMA encoding='UTF-16le'` | Python sqlite3 | `make_data.py` |
| `dbfpy_dbase3.dbf` | dBase III, code page 1252: NAME C(20), QTY N(5,0), PRICE N(8,2), BORN D, OK L; rows Tea/3/1.50/2024-05-01/T, Café/-12/0.99/1999-12-31/F, an empty row and a deleted row | dbf 0.99.11 | `make_data.py` |
| `dbfpy_foxpro.dbf` | Visual FoxPro: NAME C(10), COUNT I, AMOUNT Y, STAMP T, RATIO B, D D; rows alpha/7/12.3456/2024-05-01 12:30:15/2.5/2024-05-01 and beta/-1/-0.5/2000-01-01 00:00:00/-0.001/1970-01-01 | dbf 0.99.11 | `make_data.py` |
| `tabulate.md`, `.rst`, `.simple.rst`, `.org`, `.jira`, `.wiki`, `.tex`, `.booktabs.tex`, `.html`, `.adoc`, `.psql.txt`, `.box.txt` | the table `Item Qty Price Note` / `Tea 3 1.5 green` / `Grüße -12 0.99 世界` / `pipe\|char 0 1e-05` in the formats github, grid, rst, orgtbl, jira, mediawiki, latex, latex_booktabs, html, asciidoc, psql and simple_grid | tabulate 0.10.0 | `make_data.py` |
| `pandas_sample_v118.dta`, `pandas_sample_v114.dta` | 3 rows: id int32 1..3, score 1.5/-2.25/missing, label alpha/beta/gamma, when 2024-05-01 12:30, 1999-12-31, 1960-02-29 06:00 (%tc) | pandas 3.1.1 | `frame.to_stata(..., version=118 / 114, time_stamp=2000-01-01)` |
| `pyreadstat_sample.sav` | the same data as SPSS (UTF-8, DATETIME format) | pyreadstat 1.3.8 | `pyreadstat.write_sav(frame, ...)` |
| `pyreadstat_sample.xpt` | id, score, label as SAS XPORT v5, data set name SAMPLE | pyreadstat 1.3.8 | `pyreadstat.write_xport(plain, ..., table_name="SAMPLE", file_format_version=5)` |

### Delimited text (`csv/`)

Written with Python 3.14's `csv` module by `python3 tools/make_csv.py csv`. The
rows of `excel_crlf.csv`, `unix_all_quoted.csv`, `excel_tab.tsv`, `pipe.psv` and
`utf16le_bom.tsv`: titles `name qty price when ok note`, then
`Tea 3 1.50 2024-05-01 true plain`, `"Milk, oat" 12 0.99 2024-05-02T10:30:00 false says "hi"`,
`Grüße 世界 -7 1e-05 (empty) TRUE line one⏎line two`, `007 0 (empty) 1999-12-31 (empty) (empty)`,
`"  padded  " 12345678901234567890 3.14159 2024-02-29 23:59:59 FALSE tab⇥here`.

| File | Dialect |
| --- | --- |
| `excel_crlf.csv` | `csv.writer` default (excel: comma, minimal quoting, CRLF) |
| `unix_all_quoted.csv` | `dialect="unix"` (every field quoted, LF) |
| `excel_tab.tsv` | `dialect="excel-tab"` |
| `pipe.psv` | `delimiter="\|"`, LF |
| `utf16le_bom.tsv` | excel-tab, `encoding="utf-16"` (UTF-16LE with BOM, Excel's "Unicode text") |
| `utf16be.csv` | first three rows, `encoding="utf-16-be"` (no BOM) |
| `semicolon_bom.csv` | `Artikel Menge Preis` / `Käse 2 3,50` / `Brot; dunkel 1 2,25` / `Wein -1 12,00`, `delimiter=";"`, `encoding="utf-8-sig"` |
| `sep_hint.csv` | the same rows after an Excel `sep=;` line |
| `cp1252.csv` | `word price` / `café 4` / `naïve 5` / `€uro 6` in Windows-1252 |
| `backslash_escape.csv` | `escapechar="\\", doublequote=False, quoting=QUOTE_NONNUMERIC`: `id text` / `1 he said "yes"` / `2 back\slash` |
| `ragged.csv` | rows `a,b,c` / `1` / `2,3,4,5` / (empty) / `6,7`, no final line break |
| `report.fwf` | a fixed-width report made with `str.ljust`/`str.rjust`: `Item Qty Price Date` / `Tea 3 1.50 2024-05-01` / `Milk oat 12 0.99 2024-05-02` / `Grüße -7 100.00 1999-12-31` |

## Interoperability of Convertible's output

Checked while developing with `tools/interop_check.py` in the same container
(files written by the dev build from `lo_sample.xlsx`, `openpyxl_big_10k.xlsx`
and `xlwt_biff8.xls`):

- **openpyxl 3.1.5** opens our `.xlsx`, `.xlsm` and `.xltx`: four sheets, "Hidden"
  hidden, values typed (dates as `datetime`, times as `time`), A1:C1 and B4:B5 merged,
  frozen header at A2; the 10,000-row sheet and the re-written xlwt workbook too.
- **xlrd 2.0** opens our `.xls` (BIFF8): sheet names, visibility, numbers, dates through
  the date formats, the merged range, and the long and wide strings of a shared
  string table that spans CONTINUE records, character for character.
- **LibreOffice 24.2** converts our `.xlsx`, `.xls`, `.ods`, `.fods`, SpreadsheetML 2003,
  `.html`, `.dbf` and `.csv` back to CSV with the expected values on every sheet.
- **pyarrow 26.0.1** reads our Parquet files (uncompressed, Snappy, gzip) with the
  schema string/int64/double/bool/date32/timestamp[us]/time64[us].
- **Python sqlite3 / sqlite3 3.54 CLI**: `PRAGMA integrity_check` is `ok` for our databases
  (including 10,000 rows with interior pages); our SQL script for SQLite executes as is.
- **dbf 0.99.11** reads our dBase III files with the inferred field types.

The Arrow fixtures were made with `tools/make_arrow.py` in the same container.

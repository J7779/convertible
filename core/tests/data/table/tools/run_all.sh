#!/bin/sh
# Regenerates the spreadsheet and data fixtures of this directory inside a
# throwaway container built from ./Dockerfile:
#
#   docker build -t table-fixtures tools
#   docker run --rm --memory 2g --user 12345 -e HOME=/tmp -v "$PWD/out":/w -v "$PWD/tools":/g table-fixtures sh /g/run_all.sh
#
# Running under a numeric user id that has no account makes the writers that
# record a "last saved by" name (Gnumeric's BIFF export) write "Unknown".
# /w must already contain sample_compact.ods (see SOURCES.md, lo_sample_2003.xml).
set -e
mkdir -p /tmp/home
export HOME=/tmp/home
cd /w
/venv/bin/pip install -q tabulate dbf >/dev/null 2>&1
/venv/bin/python /g/make_workbooks.py /w
S=sample_openpyxl.xlsx
soffice --headless --convert-to xlsx:"Calc MS Excel 2007 XML" --outdir lo $S >/dev/null 2>&1
soffice --headless --convert-to xls:"MS Excel 97" --outdir lo $S >/dev/null 2>&1
soffice --headless --convert-to ods --outdir lo $S >/dev/null 2>&1
soffice --headless --convert-to fods --outdir lo $S >/dev/null 2>&1
soffice --headless --convert-to html:"HTML (StarCalc)" --outdir lo $S >/dev/null 2>&1
soffice --headless --convert-to slk:"SYLK" --outdir lo $S >/dev/null 2>&1
soffice --headless --convert-to dif:"DIF" --outdir lo $S >/dev/null 2>&1
soffice --headless --convert-to "xml:MS Excel 2003 XML" --outdir lo sample_compact.ods >/dev/null 2>&1
ssconvert $S gnm.gnumeric >/dev/null 2>&1
ssconvert -T Gnumeric_Excel:excel_biff7 $S gnm_biff5.xls >/dev/null 2>&1
ssconvert -T Gnumeric_sylk:sylk $S gnm.slk >/dev/null 2>&1
ssconvert -T Gnumeric_dif:dif $S gnm.dif >/dev/null 2>&1
mkdir -p data
/venv/bin/python /g/make_data.py /w/data
ls -la /w /w/lo /w/data

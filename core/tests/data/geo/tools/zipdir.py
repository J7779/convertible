# Copyright (c) Convertible contributors. MIT licence; see LICENSE.
# Deterministic ZIP of the given files (stored names without directories,
# timestamps 1980-01-01, no extra fields):
#   python3 zipdir.py out.zip file...
import os
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    for path in sys.argv[2:]:
        info = zipfile.ZipInfo(os.path.basename(path), date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        with open(path, "rb") as f:
            z.writestr(info, f.read())

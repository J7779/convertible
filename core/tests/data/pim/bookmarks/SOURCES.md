# Sources: pim/bookmarks

Written by `gen_pim_b.py` (Python 3.14 standard library): `python -I gen_pim_b.py out`.
All URLs use example.org / example.com; all titles are made up.

| File | Tool | Contents |
| --- | --- | --- |
| `netscape.html` | string formatting (the format every browser exports) | folder "Bookmarks bar" (ADD_DATE 1700000000) with "Example & Co" (TAGS example,docs; `<DD>` description) and sub-folder "Ünïcödé folder" holding "שלום 🎉"; a top-level "Other bookmark" with `&amp;` in its URL |
| `chrome_bookmarks.json` | `json.dumps` | Chrome/Edge layout: roots bookmark_bar / other / synced, date_added in microseconds since 1601 |
| `firefox_backup.json` | `json.dumps` | Firefox backup: places containers, dateAdded in microseconds since 1970, tags, a separator |
| `example.url` | `configparser` | `[InternetShortcut] URL=...` with CRLF |
| `example.webloc` | `plistlib.dumps(..., fmt=FMT_XML)` | `{"URL": ...}` |
| `example_binary.webloc` | `plistlib.dumps(..., fmt=FMT_BINARY)` | `{"URL": "https://www.example.org/ünï"}` (UTF-16 string object) |
| `example.desktop` | `configparser` | `[Desktop Entry] Type=Link Name URL Comment Icon` |

Interoperability (development): `.webloc` and `.url` files written by
Convertible were read back with `plistlib.load` and `configparser`.

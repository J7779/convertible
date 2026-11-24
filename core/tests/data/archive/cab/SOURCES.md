# Cabinet and Compiled HTML Help fixtures

Made in the archive tools container (Debian 12, linux/amd64, root, `TZ=UTC`)
from the synthetic tree of `../make_tree.py` (`t/`, all times 2021-06-15T12:00:00Z).

| File | Content | Tool | Command |
| --- | --- | --- | --- |
| `plain.cab` | `hello.txt`, `lorem.txt`, one stored folder | gcab 1.5 | `cd t && gcab -c -n ../plain.cab hello.txt lorem.txt` |
| `mszip.cab` | `hello.txt`, `lorem.txt`, `noise.bin`, one MSZIP folder | gcab 1.5 | `cd t && gcab -c -z -n ../mszip.cab hello.txt lorem.txt noise.bin` |
| `demo.chm` | `index.html`, `lorem.html`, `sub/page.html` (LZX, ITSF version 3) | chmcmd 3.2.2 (Free Pascal) | see below |

The CHM project (`a.hhp` and three pages, all synthetic):

```sh
mkdir -p c/sub && cd c
printf '[OPTIONS]\nCompatibility=1.1 or later\nCompiled file=demo.chm\nDefault topic=index.html\nLanguage=0x409 English (United States)\nTitle=Demo\n\n[FILES]\nindex.html\nlorem.html\nsub/page.html\n' > a.hhp
printf '<html><body><h1>Index</h1><p>Hello, archive!</p></body></html>\n' > index.html
printf '<html><body><p>Sub page</p></body></html>\n' > sub/page.html
python3 -c "import make_tree; open('lorem.html','w').write('<html><body><pre>'+make_tree.LOREM*40+'</pre></body></html>\n')"
chmcmd a.hhp
```

The tests also build small stored cabinets in memory (several folders,
reserved header/folder/data areas, UTF-8 and Latin-1 names, attributes, and a
cabinet in the middle of a set with files continued from the previous and
into the next cabinet), because none of the available tools writes those.

No fixture contains a user name, host name, absolute path or the date it was
made (scanned with `grep -aiE` for the login name, `/Users`, `/home/`,
`.local`, `/w/`, `/tmp/`, `build` and the year of the run).

## Interop of our output

CAB and CHM are read only.

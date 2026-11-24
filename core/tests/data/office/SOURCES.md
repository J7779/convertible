# Office family fixtures

All content is synthetic (lorem ipsum, made-up widgets and prices,
example.com/.org addresses). Most fixtures are made from one source document
by several independent programs, so every reader is checked against the
same known structure. Tools ran in a throwaway Docker container (Debian 13
with LibreOffice 25.2.3.2, pandoc 3.1.11.1, Calibre 8.5.0, python-docx 1.2.0,
python-pptx 1.0.2, AbiWord 3.0.6, epubcheck 4.2.6) started with
`HOME=/tmp/home LANG=C.UTF-8 TZ=UTC SOURCE_DATE_EPOCH=0`, LibreOffice with a
fresh profile (`-env:UserInstallation=file:///tmp/lo`, preview thumbnails
off), so no account name reaches the files; textutil ran on macOS 26.
Commands are run from the directory named in each section unless stated.

After generation every file was scanned with
`LC_ALL=C grep -r -l -a -i -E "<login>|/Users|/home/|\.local" .` (no
matches) and the author fields of `docProps/core.xml`, `docProps/app.xml`,
`meta.xml`, `\info`, `o:DocumentProperties`, AbiWord metadata and the
SummaryInformation streams were checked: they are empty or absent.

## base/ - the sources

| File | Content | Made with |
| --- | --- | --- |
| `sample.md` | Pandoc Markdown written by hand: title "Office Sample", lang en-GB; headings "Quarterly Widget Report" (1), "Lists and Steps", "Inventory Table", "Code and Quotes" (2), "Unicode Corner" (3); bold/italic/inline code; a bullet list of 3 with 2 nested items; an ordered list of 3; a 3x4 table (Part/Count/Price: Sprocket 4 2.50, Gear 16 0.75, Spring 120 0.05); a code block; a block quote; Latin/Greek/CJK/Hebrew/Arabic text and emoji; one footnote ("The note text lives at the bottom."); super/subscript; one picture; links to https://example.com/widgets and https://example.org/page | text editor |
| `sample.html` | The same document as HTML, plus a table with a colspan and a rowspan, a nested table, underline, an RTL paragraph; the picture is an inline data: URI | text editor (picture inlined with Python's base64) |
| `picture.png` | 16x12 RGB gradient #3366cc to #ffcc00 | ImageMagick 7: `magick -size 16x12 gradient:"#3366cc"-"#ffcc00" -strip PNG24:picture.png` |
| `photo.jpg` | 240x180 plasma fractal, JPEG quality 98 (for the "compress" tests) | `magick -seed 11 -size 240x180 plasma:fractal -strip -quality 98 photo.jpg` |
| `photo.md` | A heading, two paragraphs and `photo.jpg` | text editor |
| `chm/` | HTML Help project for `chmcmd/sample.chm`: `sample.hhp`, contents `toc.hhc`, index `index.hhk`, four topics (`intro.htm`, `topics/*.htm`) and `images/logo.png` | text editor |
| `chm_cyrillic/` | HTML Help project for `chmcmd/cyrillic.chm`, kept in UTF-8 (one Russian page, its contents entry and title) | text editor |
| `lit/` | OPF package for `calibre/sample.lit`: `book.opf` (title "Widget Almanac", author "Jane Example", publisher, description, subject, date), `ch1.html` (headings, lists, a table with spans, `../picture.png`, a link to `ch2.html`), `ch2.html` (Unicode, code, quote, definition list, a link to `ch1.html#inventory`) | text editor |
| `deck.py` | python-pptx script building the sample deck (5 slides: title slide; title + bullets with a nested level + speaker notes "Remember to smile."; a 2x3 table; a picture with alt text "Gradient swatch"; a group with a bold text box) | text editor |
| `doc_comments.py` | python-docx script: heading "Review Notes", a comment "Check the numbers again." by "Reviewer", a numbered list, header "Header line", footer "Footer line" | text editor |
| `make_xps.py` | Python (stdlib zipfile) script writing `handmade/sample.xps` per the XPS specification | text editor |

## pandoc/ (pandoc 3.1.11.1, from base/)

| File | Command |
| --- | --- |
| `sample.docx` | `pandoc sample.md -s -t docx -o ../pandoc/sample.docx` |
| `sample.odt` | `pandoc sample.md -s -t odt -o ../pandoc/sample.odt` |
| `sample.rtf` | `pandoc sample.md -s -t rtf -o ../pandoc/sample.rtf` |
| `sample.epub` | `pandoc sample.md -s -t epub3 -o ../pandoc/sample.epub` |
| `sample_epub2.epub` | `pandoc sample.md -s -t epub2 -o ../pandoc/sample_epub2.epub` |
| `sample.pptx` | `pandoc sample.md -s -t pptx -o ../pandoc/sample.pptx` |
| `photo.docx` | `pandoc photo.md -s -t docx -o ../pandoc/photo.docx` |
| `photo.epub` | `pandoc photo.md -s -t epub3 -o ../pandoc/photo.epub` |
| `sample.fbz` | `pandoc sample.md -t fb2 -o ../pandoc/sample.fb2`, then in pandoc/: `zip -X -q sample.fbz sample.fb2` (Info-ZIP 3.0) |

## libreoffice/ (LibreOffice 25.2.3.2 headless)

`lo` stands for `soffice -env:UserInstallation=file:///tmp/lo --headless`.

| File | Command (from base/) |
| --- | --- |
| `sample.docx` | `lo --infilter="HTML (StarWriter)" --convert-to docx:"MS Word 2007 XML" --outdir ../libreoffice sample.html` |
| `sample.doc` | `lo --infilter="HTML (StarWriter)" --convert-to doc:"MS Word 97" --outdir ../libreoffice sample.html` |
| `sample.rtf` | `lo --infilter="HTML (StarWriter)" --convert-to rtf:"Rich Text Format" --outdir ../libreoffice sample.html` |
| `sample.odt` | `lo --infilter="HTML (StarWriter)" --convert-to odt:"writer8" --outdir ../libreoffice sample.html` |
| `sample.fodt` | `lo --infilter="HTML (StarWriter)" --convert-to fodt:"OpenDocument Text Flat XML" --outdir ../libreoffice sample.html` |
| `sample.epub` | `lo --infilter="HTML (StarWriter)" --convert-to epub:"EPUB" --outdir ../libreoffice sample.html` |
| `notes.doc` | pandoc's `sample.docx` copied to `notes.docx`, then `lo --convert-to doc:"MS Word 97" --outdir ../libreoffice notes.docx` (footnote, code, quote) |
| `sample.odp` | `lo --convert-to odp:"impress8" --outdir ../libreoffice ../python/sample.pptx` |
| `sample_impress.pptx` | `lo --convert-to pptx:"Impress MS PowerPoint 2007 XML" --outdir /tmp/lopptx ../python/sample.pptx`, renamed |
| `sample.ppt.gz` | `lo --convert-to ppt:"MS PowerPoint 97" --outdir /tmp/ppt ../python/sample.pptx`, then `gzip -9 -n -c /tmp/ppt/sample.ppt > ../libreoffice/sample.ppt.gz`. LibreOffice stores a 588 KB preview bitmap in the SummaryInformation stream; gzip makes it 21 KB. The tests gunzip it (byte-identical to LibreOffice's output). |

LibreOffice's HTML import puts the nested table into the first cell of the
outer table and expresses merged cells through cell widths in RTF and DOC;
the readers reproduce exactly that.

## python/ (python-docx 1.2.0, python-pptx 1.0.2, in a venv)

| File | Command (from base/) |
| --- | --- |
| `sample.pptx` | `python deck.py` |
| `comments.docx` | `python doc_comments.py` |

## calibre/ (Calibre 8.5.0 ebook-convert, from pandoc/)

| File | Command |
| --- | --- |
| `sample.mobi` | `ebook-convert sample.epub ../calibre/sample.mobi --mobi-file-type old` (MOBI 6, PalmDOC compression) |
| `sample.azw3` | `ebook-convert sample.epub ../calibre/sample.azw3` (KF8: FDST, skeleton and fragment indexes) |
| `sample_palmdoc.pdb` | `ebook-convert sample.epub ../calibre/sample_palmdoc.pdb --format doc` (PalmDOC "TEXtREAd") |
| `sample.tcr` | `ebook-convert sample.epub ../calibre/sample.tcr` |

Microsoft Reader and the help-file readers were checked against Calibre 7.6.0
(Ubuntu 24.04 package) in a throwaway container started with
`HOME=/tmp/home LANG=C.UTF-8 TZ=UTC`:

| File | Command |
| --- | --- |
| `sample.lit` | from `base/lit/` with `../picture.png` copied next to the sources: `ebook-convert book.opf sample.lit` (Calibre's LIT writer: LZX-compressed content section, "sealed" level-1 book without encrypted content; Calibre splits `ch1.html` into three pages at the headings) |
| `sample_lit.txt` | `ebook-convert sample.lit sample_lit.txt`: Calibre's own reading of `sample.lit`, the reference for the text-similarity check |
| `sample_chm.txt` | `ebook-convert ../chmcmd/sample.chm sample_chm.txt`: Calibre's reading of the help file (it drops the contents' folder headings) |

No other program writes LIT files today (Microsoft's own tools are long
gone), so the attribute numbers of the binary markup were established from
`sample.lit` and from a scratch book with many attributes (not kept); the
element numbers follow the MSHTML element ids. `help_ebook_test.cpp` also
assembles minimal LIT containers itself (stored section only) to test
hand-written binary markup and the DRM refusals. Calibre 7.6.0 reads
`cyrillic.chm` as an empty book; 7-Zip 23.01 (`7z x`) extracts its page,
which is byte-identical to `base/chm_cyrillic/page.htm` converted with
`iconv -f UTF-8 -t CP1251`, so the expected text comes from that source.

## chmcmd/ (Free Pascal chmcmd 3.2.2, Ubuntu 24.04 package fp-utils)

| File | Command |
| --- | --- |
| `sample.chm` | in `base/chm/`: `chmcmd sample.hhp` (contents with a folder node and nested entries, keyword index, a topic reachable only by a link, a picture, an `<a name>` target; LCID 0x409) |
| `cyrillic.chm` | in a copy of `base/chm_cyrillic/` whose three files were converted with `iconv -f UTF-8 -t CP1251`: `chmcmd cyrillic.hhp` (LCID 0x419; the page declares no charset) |

`base/chm/images/logo.png`: `convert -size 16x12 gradient:#3366cc-#ffcc00 -strip PNG24:images/logo.png` (ImageMagick 6.9.12).

## textutil/ (macOS 26 textutil, from base/)

| File | Command |
| --- | --- |
| `sample.docx` | `textutil -convert docx sample.html -output ../textutil/sample.docx` (textutil drops tables and links) |
| `sample.doc` | `textutil -convert doc sample.html -output ../textutil/sample.doc` |
| `sample.rtf` | `textutil -convert rtf sample.html -output ../textutil/sample.rtf` |
| `sample.odt` | `textutil -convert odt sample.html -output ../textutil/sample.odt` |
| `sample.wordml` | `textutil -convert wordml sample.html -output ../textutil/sample.wordml` (Word 2003 XML) |

## abiword/ (AbiWord 3.0.6, Debian package)

| File | Command (from libreoffice/) |
| --- | --- |
| `sample.abw` | `abiword --to=abw --to-name=../abiword/sample.abw sample.odt` (AbiWord turns both lists of the source into one bullet list) |

## handmade/

| File | Command (from base/) |
| --- | --- |
| `sample.xps` | `python3 make_xps.py` (Python 3.13 stdlib): two fixed pages, Glyphs runs at known positions (title 28 pt, body 14 pt, one run in a translated Canvas), one ImageBrush with `picture.png`. Checked with MuPDF `mutool draw -F txt` (Debian mupdf-tools), which reads the same text in the same order (with a fallback font: the package names no font file). |

There is no free XPS writer; the package was written by hand against the
specification and verified with an independent reader.

## archives/

Pages made with ImageMagick 7: `magick -size 40x60 xc:"#cc3333" -strip PNG24:page1.png`,
the same with `#33cc33` for `page2.png` and `#3333cc` for `page10.png`.

| File | Command |
| --- | --- |
| `comic.cbz` | `zip -X -q -0 comic.cbz page10.png page2.png page1.png` (stored out of order on purpose) |
| `comic.cbt` | `tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime=@0 -cf comic.cbt page1.png page2.png page10.png` (GNU tar 1.35) |

The nested comics share one source tree, made with ImageMagick 6.9.12
(`convert -size 40x60 xc:<colour> -strip PNG24:<file>`): `cover.png`
(#eeeeee), `Chapter 1/page1.png` (#cc3333), `Chapter 1/page2.png` (#33cc33),
`Chapter 1/page10.png` (#3333cc), `Chapter 2/page1.png` (#cccc33), plus the
clutter `notes.txt` ("Scanned for testing.") and a `ComicInfo.xml` with the
title "Widget Comics"; every time stamp set with `touch -h -d @0`. Members
are added out of order on purpose.

| File | Command (in the source tree) |
| --- | --- |
| `comic.cbr` | `rar a -ma5 -m5 -ts- ../comic.cbr cover.png 'Chapter 2' 'Chapter 1' notes.txt ComicInfo.xml` (RAR 7.00, RAR 5 format; Ubuntu 24.04 amd64 package) |
| `comic.cb7` | `7z a -t7z -mx=9 -mtm=off -mtc=off -mta=off ../comic.cb7 cover.png 'Chapter 2' 'Chapter 1' notes.txt ComicInfo.xml` (7-Zip 23.01) |
| `comic_nested.cbt` | `tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime=@0 -cf ../comic_nested.cbt 'Chapter 2' notes.txt 'Chapter 1' cover.png ComicInfo.xml` (GNU tar 1.35) |

## Inputs built inside the tests

No independent program writes Outlook MSG, TNEF (winmail.dat), HWP 5,
HWPX or VSDX files, so `containers_test.cpp` assembles small ones from the
specifications ([MS-OXMSG], [MS-OXTNEF], the Hancom HWP 5.0 and OWPML
documents, [MS-VSDX]). Their bytes were written to files once and checked
with independent readers: `extract_msg` 0.58.3 (Python) and `msgconvert`
(Email::Outlook::Message) read the subject, sender, date, HTML body and both
attachments of the MSG; `tnef --list` (tnef 1.4.18) lists `report.txt` in the
TNEF stream. The compressed-RTF test vector is the example of
[MS-OXRTFCP] section 3.1.1.

## Interoperability of our output

Files written by the office writers from `base/sample.md`, the pandoc DOCX,
the LibreOffice DOC and the python-pptx deck were opened with independent
software:

| Writer | Check | Result |
| --- | --- | --- |
| DOCX | LibreOffice 25.2 `--convert-to txt`; python-docx `Document()`; macOS textutil `-convert txt` | opens without repair, same text, lists, table and picture count |
| RTF | LibreOffice `--convert-to txt`; macOS textutil | same text and lists |
| ODT, flat ODT | LibreOffice `--convert-to txt` | same text and lists |
| Word 2003 XML | LibreOffice with `--infilter="MS Word 2003 XML"` | same text and lists |
| PPTX | LibreOffice `--convert-to pdf` + `pdftotext`; python-pptx `Presentation()` | 5 slides with the expected titles, bullets, table |
| ODP | LibreOffice `--convert-to pdf` + `pdftotext` | slides, bullets, table frame |
| EPUB | epubcheck 4.2.6 (EPUB 3.2 rules); Calibre `ebook-convert` to TXT | 0 errors, 0 warnings; text complete |
| MOBI | Calibre `ebook-convert` to TXT | text complete |

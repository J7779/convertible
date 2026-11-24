# Sources: pim/bibliography

Written by `gen_bib.py` in a scratch virtual environment (Python 3.14,
bibtexparser 1.4.4, pybtex 0.26.1, rispy 0.10.0): `python -I gen_bib.py bib`.
All references are fictional; DOIs use the made-up prefix 10.1234.

The two records most files hold:

1. Journal article: Ada Example and Jean-Luc de la Fontaine, "A synthetic
   study of ünïcödé handling", Journal of Made-up Results 12(3), 101-115,
   March 2021, DOI 10.1234/example.2021.001, keywords testing, unicode.
2. Book: Ben Sample Jr., "Fictional Methods", Example Press, Springfield,
   2019, ISBN 978-0-00-000000-0.

| File | Tool | Notes |
| --- | --- | --- |
| `bibtexparser.bib` | bibtexparser `BibTexWriter().write(db)` | both records; LaTeX accents `{\"u}`, `month = {mar}`, `Sample, Jr., Ben` |
| `pybtex.bib` | pybtex `BibliographyData.to_string("bibtex")` | an `@inproceedings` with a corporate author `{Example Consortium}`, an editor and `{DNA}` |
| `macros.bib` | hand-written | `@string` macros, `#` concatenation, `@comment`, `@preamble`, BibLaTeX `journaltitle`/`date`/`langid`/`urldate`, `{\O}`, `\`e`, `---`, `\emph`, math, quoted value with ``` `` '' ``` |
| `rispy.ris` | rispy `rispy.dumps(entries)` | both records, numbered (`1.`) as rispy writes them |
| `items.csl.json` | `json.dumps` | both records as CSL-JSON (particle "de la", suffix "Jr.") |
| `pubmed.nbib` | hand-written | MEDLINE record with PMID, a wrapped title, FAU/AU, DP, LID [doi], MH/OT |
| `endnote.enw` | hand-written | EndNote tagged records |
| `refworks.txt` | hand-written | RefWorks tagged records |
| `wos.isi` | hand-written | Web of Science export with continuation lines for AU/AF/TI |
| `endnote.xml` | `xml.etree.ElementTree` | EndNote XML record with `<style>` runs |
| `records.mods.xml` | `xml.etree.ElementTree` | MODS collection with a host `relatedItem` |

Interoperability (development): BibTeX and BibLaTeX written by Convertible
from `items.csl.json` (`convertible convert items.csl.json -t bibtex
[--opt bib_dialect=biblatex]`) parse in bibtexparser (with
`convert_to_unicode`) and pybtex with identical keys, titles, authors and
years; RIS output parses in rispy with identical types, authors, titles and
pages.

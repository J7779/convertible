# Sources: util/xml

Fixtures for `core/tests/util/xml_test.cpp`. All content is synthetic. Every
file was written by Python 3.14.10 (its `xml.etree.ElementTree` and
`xml.dom.minidom` modules, built on expat 2.9.0), never by Convertible's own
XML writer. Run the commands in this directory; each one recreates one file
byte for byte.

`.gitattributes` (`* -text`) keeps git from translating line endings here.

| File | Contents |
| --- | --- |
| `elementtree_namespaces.xml` | 347 bytes, UTF-8. A `catalog` in the default namespace `urn:example:catalog` with a second namespace `urn:example:meta` (prefix `m`): two `item` elements (`id` `a1` / `a2`, `price` `12.50` / `0.75`), escaped text `Blue <widget> & "gadget"`, mixed content `Mixed <b>bold</b> tail text`, a prefixed attribute `m:note` holding a tab written as `&#09;`, and an empty `m:empty`. |
| `minidom_pretty.xml` | 314 bytes, UTF-8, indented by two spaces. Comment and processing instruction before the root, `report` with `title="Q&A <summary>"`, a `para`, a CDATA section `if (a < b && c > d) { run(); }`, an empty `break` and a trailing comment. |
| `utf16_bom_elementtree.xml` | 372 bytes, UTF-16 little endian with byte order mark, declared `encoding='utf-16'`. Three `greeting` elements: `Hello`, Japanese `こんにちは`, and `€ 5 😀` (a character outside the BMP, i.e. a surrogate pair). |
| `latin1_elementtree.xml` | 140 bytes, ISO-8859-1 as declared. Non-ASCII attribute name and value (`café="crème brûlée"`), text `Æbleskiver à la carte, señor`, and a euro sign written as `&#8364;` because Latin-1 cannot encode it. |
| `doctype_internal_subset.xml` | 457 bytes, UTF-8. A `<!DOCTYPE note [...]>` whose internal subset holds a comment and a processing instruction containing `]` and `>`, an element and an attribute-list declaration, and three internal entities (`product` = `Sample`, `greeting` = `Hello from &product;`, `copy` = `&#169;`) that the document uses in text and in an attribute. |
| `ooxml_like_document.xml` | 880 bytes, UTF-8. Shaped like a WordprocessingML `document.xml` (prefixes `w` and `r` with the real OOXML namespace URIs, lorem-ipsum content): three paragraphs, runs with `xml:space="preserve"` text (`Lorem ipsum `, bold `dolor`, ` sit amet.`, `  two leading spaces`), a hyperlink with `r:id="rId4"`, and a `w:pgSz` with `w:w="11906"` `w:h="16838"`. |

## Commands

`elementtree_namespaces.xml`

```sh
python3 - <<'EOF'
import xml.etree.ElementTree as ET
ET.register_namespace("", "urn:example:catalog")
ET.register_namespace("m", "urn:example:meta")
C = "{urn:example:catalog}"
M = "{urn:example:meta}"
root = ET.Element(C + "catalog", {"version": "3", M + "checked": "true"})
root.text = "\n  "
first = ET.SubElement(root, C + "item", {"id": "a1", "price": "12.50"})
first.text = 'Blue <widget> & "gadget"'
first.tail = "\n  "
second = ET.SubElement(root, C + "item", {"id": "a2", "price": "0.75", M + "note": "tab\there, quote \" and <angle>"})
second.text = "Mixed "
bold = ET.SubElement(second, C + "b")
bold.text = "bold"
bold.tail = " tail text"
second.tail = "\n  "
empty = ET.SubElement(root, M + "empty")
empty.tail = "\n"
ET.ElementTree(root).write("elementtree_namespaces.xml", encoding="utf-8", xml_declaration=True)
EOF
```

`minidom_pretty.xml`

```sh
python3 - <<'EOF'
import xml.dom.minidom as minidom
doc = minidom.getDOMImplementation().createDocument(None, "report", None)
root = doc.documentElement
doc.insertBefore(doc.createComment(" synthetic sample "), root)
doc.insertBefore(doc.createProcessingInstruction("render", 'mode="draft"'), root)
root.setAttribute("title", "Q&A <summary>")
section = doc.createElement("section")
section.setAttribute("n", "1")
root.appendChild(section)
para = doc.createElement("para")
para.appendChild(doc.createTextNode("alpha & beta < gamma"))
section.appendChild(para)
code = doc.createElement("code")
code.appendChild(doc.createCDATASection("if (a < b && c > d) { run(); }"))
section.appendChild(code)
section.appendChild(doc.createElement("break"))
root.appendChild(doc.createComment(" end of report "))
open("minidom_pretty.xml", "wb").write(doc.toprettyxml(indent="  ", encoding="utf-8"))
EOF
```

`utf16_bom_elementtree.xml`

```sh
python3 - <<'EOF'
import xml.etree.ElementTree as ET
root = ET.Element("greetings", {"count": "3"})
for lang, text in (("en", "Hello"), ("ja", "こんにちは"), ("emoji", "€ 5 \U0001F600")):
    ET.SubElement(root, "greeting", {"lang": lang}).text = text
ET.ElementTree(root).write("utf16_bom_elementtree.xml", encoding="utf-16", xml_declaration=True)
EOF
```

`latin1_elementtree.xml`

```sh
python3 - <<'EOF'
import xml.etree.ElementTree as ET
root = ET.Element("menu", {"café": "crème brûlée"})
ET.SubElement(root, "plat").text = "Æbleskiver à la carte, señor"
ET.SubElement(root, "prix").text = "4 €"
ET.ElementTree(root).write("latin1_elementtree.xml", encoding="iso-8859-1", xml_declaration=True)
EOF
```

`doctype_internal_subset.xml` (ElementTree and minidom cannot write an
internal subset, so the bytes are written literally)

```sh
python3 - <<'EOF'
import pathlib
pathlib.Path("doctype_internal_subset.xml").write_bytes(b"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE note [
  <!-- internal subset: a ] and a > inside a comment -->
  <!ELEMENT note (to, body)>
  <!ATTLIST note lang CDATA #IMPLIED title CDATA "untitled ]>">
  <!ENTITY product "Sample">
  <!ENTITY greeting "Hello from &product;">
  <!ENTITY copy "&#169;">
  <?subset-pi ]> ?>
]>
<note lang="en" title="&product; &copy;">
  <to>&greeting;</to>
  <body>Line one&#10;line two &amp; &lt;three&gt;</body>
</note>
""")
EOF
```

`ooxml_like_document.xml`

```sh
python3 - <<'EOF'
import xml.etree.ElementTree as ET
W = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
XML = "http://www.w3.org/XML/1998/namespace"
ET.register_namespace("w", W)
ET.register_namespace("r", R)
def w(tag): return "{%s}%s" % (W, tag)
doc = ET.Element(w("document"))
body = ET.SubElement(doc, w("body"))
for index, runs in enumerate(((("Lorem ipsum ", None), ("dolor", "b"), (" sit amet.", None)), (("  two leading spaces", None),))):
    p = ET.SubElement(body, w("p"), {w("rsidR"): "00A%d0001" % index})
    for text, style in runs:
        r = ET.SubElement(p, w("r"))
        if style:
            ET.SubElement(ET.SubElement(r, w("rPr")), w(style))
        t = ET.SubElement(r, w("t"), {"{%s}space" % XML: "preserve"})
        t.text = text
link = ET.SubElement(ET.SubElement(body, w("p")), w("hyperlink"), {"{%s}id" % R: "rId4"})
ET.SubElement(ET.SubElement(link, w("r")), w("t")).text = "link"
sect = ET.SubElement(body, w("sectPr"))
ET.SubElement(sect, w("pgSz"), {w("w"): "11906", w("h"): "16838"})
ET.indent(doc, space="  ")
ET.ElementTree(doc).write("ooxml_like_document.xml", encoding="UTF-8", xml_declaration=True)
EOF
```

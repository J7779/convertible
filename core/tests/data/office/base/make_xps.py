"""Writes sample.xps by hand (stdlib zipfile) following the XPS specification:
one fixed document with two pages of Glyphs text and one picture."""
import zipfile

CT = '''<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="fdseq" ContentType="application/vnd.ms-package.xps-fixeddocumentsequence+xml"/>
<Default Extension="fdoc" ContentType="application/vnd.ms-package.xps-fixeddocument+xml"/>
<Default Extension="fpage" ContentType="application/vnd.ms-package.xps-fixedpage+xml"/>
<Default Extension="png" ContentType="image/png"/>
<Default Extension="ttf" ContentType="application/vnd.ms-opentype"/>
</Types>'''
RELS = '''<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="R1" Type="http://schemas.microsoft.com/xps/2005/06/fixedrepresentation" Target="/FixedDocumentSequence.fdseq"/>
</Relationships>'''
SEQ = '''<FixedDocumentSequence xmlns="http://schemas.microsoft.com/xps/2005/06"><DocumentReference Source="/Documents/1/FixedDocument.fdoc"/></FixedDocumentSequence>'''
DOC = '''<FixedDocument xmlns="http://schemas.microsoft.com/xps/2005/06"><PageContent Source="Pages/1.fpage"/><PageContent Source="Pages/2.fpage"/></FixedDocument>'''

def glyphs(x, y, size, text):
    return '<Glyphs Fill="#FF000000" FontUri="/Resources/font.ttf" FontRenderingEmSize="%s" OriginX="%s" OriginY="%s" UnicodeString="%s"/>' % (size, x, y, text)

PAGE1 = ('<FixedPage xmlns="http://schemas.microsoft.com/xps/2005/06" Width="816" Height="1056" xml:lang="en-US">'
         + glyphs(96, 120, 28, "Quarterly Widget Report")
         + glyphs(96, 180, 14, "Lorem ipsum dolor sit amet,")
         + glyphs(300, 180, 14, "consectetur adipiscing elit.")
         + glyphs(96, 198, 14, "Sed do eiusmod tempor incididunt.")
         + '<Canvas RenderTransform="1,0,0,1,0,200">' + glyphs(96, 60, 14, "Text inside a moved canvas.") + '</Canvas>'
         + '<Path Data="M 96,320 L 256,320 256,440 96,440 Z"><Path.Fill><ImageBrush ImageSource="/Resources/picture.png" '
           'Viewbox="0,0,16,12" ViewboxUnits="Absolute" Viewport="96,320,160,120" ViewportUnits="Absolute"/></Path.Fill></Path>'
         + glyphs(96, 500, 14, "Text below the picture.")
         + '</FixedPage>')
PAGE2 = ('<FixedPage xmlns="http://schemas.microsoft.com/xps/2005/06" Width="816" Height="1056" xml:lang="en-US">'
         + glyphs(96, 120, 22, "Second Page")
         + glyphs(96, 180, 14, "Final paragraph of the sample.")
         + '</FixedPage>')

def stored(z, name, data):
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(info, data)

with zipfile.ZipFile('../handmade/sample.xps', 'w') as z:
    stored(z, '[Content_Types].xml', CT)
    stored(z, '_rels/.rels', RELS)
    stored(z, 'FixedDocumentSequence.fdseq', SEQ)
    stored(z, 'Documents/1/FixedDocument.fdoc', DOC)
    stored(z, 'Documents/1/Pages/1.fpage', PAGE1)
    stored(z, 'Documents/1/Pages/2.fpage', PAGE2)
    stored(z, 'Resources/picture.png', open('picture.png', 'rb').read())

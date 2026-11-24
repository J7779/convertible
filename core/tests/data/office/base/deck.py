"""Builds sample.pptx with python-pptx: synthetic content only."""
from pptx import Presentation
from pptx.util import Inches, Pt

prs = Presentation()
prs.core_properties.author = ""
prs.core_properties.last_modified_by = ""
prs.core_properties.title = "Widget Deck"

slide = prs.slides.add_slide(prs.slide_layouts[0])
slide.shapes.title.text = "Widget Deck"
slide.placeholders[1].text = "A synthetic presentation"

slide = prs.slides.add_slide(prs.slide_layouts[1])
slide.shapes.title.text = "Agenda Points"
body = slide.placeholders[1].text_frame
body.text = "First point"
p = body.add_paragraph()
p.text = "Nested detail"
p.level = 1
p = body.add_paragraph()
p.text = "Second point"
slide.notes_slide.notes_text_frame.text = "Remember to smile."

slide = prs.slides.add_slide(prs.slide_layouts[5])
slide.shapes.title.text = "Parts Table"
table = slide.shapes.add_table(3, 2, Inches(1), Inches(2), Inches(6), Inches(1.5)).table
for r, row in enumerate([["Part", "Count"], ["Gear", "16"], ["Spring", "120"]]):
    for c, value in enumerate(row):
        table.cell(r, c).text = value

slide = prs.slides.add_slide(prs.slide_layouts[5])
slide.shapes.title.text = "Picture Slide"
pic = slide.shapes.add_picture("picture.png", Inches(2), Inches(2), Inches(1.6), Inches(1.2))
pic._element.nvPicPr.cNvPr.set("descr", "Gradient swatch")

slide = prs.slides.add_slide(prs.slide_layouts[6])
group = slide.shapes.add_group_shape()
box = group.shapes.add_textbox(Inches(1), Inches(1), Inches(4), Inches(1))
box.text_frame.text = "Grouped text box"
run = box.text_frame.paragraphs[0].runs[0]
run.font.bold = True
run.font.size = Pt(20)

prs.save("../python/sample.pptx")

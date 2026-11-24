"""Builds comments.docx with python-docx: synthetic content only."""
import docx

d = docx.Document()
d.core_properties.author = ""
d.core_properties.last_modified_by = ""
d.add_heading("Review Notes", level=1)
p = d.add_paragraph("The widget passed ")
run = p.add_run("every test")
run.bold = True
p.add_run(" this week.")
d.add_comment(run, text="Check the numbers again.", author="Reviewer", initials="R")
d.add_paragraph("Lorem ipsum dolor sit amet.", style="List Number")
d.add_paragraph("Consectetur adipiscing elit.", style="List Number")
section = d.sections[0]
section.header.paragraphs[0].text = "Header line"
section.footer.paragraphs[0].text = "Footer line"
d.save("../python/comments.docx")

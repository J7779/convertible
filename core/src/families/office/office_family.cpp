// Convertible core - office family.
// Office documents, presentations and e-books: DOCX/DOC/RTF/ODT, PPTX/PPT/ODP,
// EPUB/MOBI/AZW3, comics, XPS, Outlook messages and their relatives.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/office/office.hpp"

namespace cvt {

void register_office_family(Registry& registry) {
  office::register_word(registry);
  office::register_doc(registry);
  office::register_rtf(registry);
  office::register_odf(registry);
  office::register_misc_text(registry);
  office::register_pptx(registry);
  office::register_ppt(registry);
  office::register_epub(registry);
  office::register_mobi(registry);
  office::register_chm_reader(registry);
  office::register_lit(registry);
  office::register_comics(registry);
  office::register_ebook_wrappers(registry);
  office::register_xps(registry);
  office::register_mail(registry);
  // Last: wraps the Document readers registered above.
  office::register_image_extraction(registry);
}

}  // namespace cvt

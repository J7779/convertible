// Convertible core - science family.
// Scientific, engineering and specialist formats: bioinformatics, chemistry,
// numerical arrays and instrument data, colour palettes, PKI containers and
// barcodes.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#include "engine/registry.hpp"
#include "families/families.hpp"
#include "families/science/science.hpp"

namespace cvt {

void register_science_family(Registry& registry) {
  science::register_sequences(registry);
  science::register_feature_tables(registry);
  science::register_alignments(registry);
  science::register_molecules(registry);
  science::register_structures(registry);
  science::register_spectra(registry);
  science::register_numpy(registry);
  science::register_mat(registry);
  science::register_netcdf(registry);
  science::register_fits(registry);
  science::register_volumes(registry);
  science::register_dicom(registry);
  science::register_hdf5(registry);
  science::register_palettes(registry);
  science::register_icc(registry);
  science::register_pki(registry);
  science::register_barcodes(registry);
  science::register_maker(registry);

  // Data codecs that share one schema: their writers accept only what their
  // group's readers produce (Codec::ir_tag), so json or yaml never offer them.
  registry.set_ir_tag({"fasta", "fastq", "gbk", "embl", "phylip", "clustal", "sto", "newick", "nex"}, Kind::Data,
                      "bio-sequences");
  registry.set_ir_tag({"xyz-molecule", "mol", "sdf", "mol2", "cml", "cif", "pdb", "mmcif"}, Kind::Data, "molecules");
  registry.set_ir_tag({"gpl", "jasc-pal", "riff-pal", "act", "aco", "ase", "palette-css", "palette-scss", "palette-less",
                       "color-tokens"},
                      Kind::Data, "palette");
  registry.set_ir_tag({"pem", "der", "p7b", "ssh-pub", "authorized-keys", "known-hosts", "jwk"}, Kind::Data, "pki");
  registry.set_ir_tag({"asc", "gpg"}, Kind::Data, "openpgp");
  // Their table views share the schema too.
  registry.set_ir_tag({"fasta", "fastq", "gbk", "embl", "phylip", "clustal", "sto", "newick", "nex"}, Kind::Table,
                      "bio-sequences");
  registry.set_ir_tag({"xyz-molecule", "mol", "sdf", "mol2", "cml", "cif", "pdb", "mmcif"}, Kind::Table, "molecules");
  registry.set_ir_tag({"gpl", "jasc-pal", "riff-pal", "act", "aco", "ase", "palette-css", "palette-scss", "palette-less",
                       "color-tokens"},
                      Kind::Table, "palette");
  // Writers that need content of their own kind straight from the source
  // (Codec::direct_only): a palette or a volume from a picture, not from a
  // rendered document or a video frame; arrays from data or tables read as
  // such; a barcode from plain text, not from a document's text.
  registry.set_direct_only({"gpl", "jasc-pal", "riff-pal", "act", "aco", "ase", "palette-css", "palette-scss", "palette-less",
                            "color-tokens", "nii", "nii.gz", "nrrd", "mha"},
                           Kind::Image);
  registry.set_direct_only({"nii", "nii.gz", "nrrd", "mha", "nc"}, Kind::Data);
  registry.set_direct_only({"nii", "nii.gz", "nrrd", "mha", "nc", "jdx"}, Kind::Table);
  registry.set_direct_only({"qr", "datamatrix", "code128", "ean13", "upca"}, Kind::PlainText);
}

}  // namespace cvt

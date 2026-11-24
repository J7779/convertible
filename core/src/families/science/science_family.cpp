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
}

}  // namespace cvt

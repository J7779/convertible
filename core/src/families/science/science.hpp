// Convertible core - science family: registration entry points.
// Copyright (c) Convertible contributors. MIT licence; see LICENSE.
#pragma once

namespace cvt {
class Registry;
}

namespace cvt::science {

void register_sequences(Registry& registry);
void register_feature_tables(Registry& registry);
void register_alignments(Registry& registry);
void register_molecules(Registry& registry);
void register_structures(Registry& registry);
void register_spectra(Registry& registry);
void register_numpy(Registry& registry);
void register_mat(Registry& registry);
void register_netcdf(Registry& registry);
void register_fits(Registry& registry);
void register_volumes(Registry& registry);
void register_dicom(Registry& registry);
void register_hdf5(Registry& registry);
void register_palettes(Registry& registry);
void register_icc(Registry& registry);
void register_pki(Registry& registry);
void register_barcodes(Registry& registry);
void register_maker(Registry& registry);

}  // namespace cvt::science

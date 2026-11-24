# Sources: science/arrays

Synthetic numerical arrays, workspaces, grids, astronomical images and volumes.
All files were written in a Python 3 virtual environment with NumPy 2.5.5,
SciPy 1.18.1, netCDF4 1.7.5.1, astropy 8.0.3, nibabel 5.5.0, pynrrd 1.1.4 and
SimpleITK 2.5.8. Commands run in this directory.

## NumPy (`gen_numpy.py`)

| File | Contents | Command |
| --- | --- | --- |
| `f8_2x3.npy` | float64 [[0, 1.5, 3], [4.5, 6, 7.5]] | `np.save("f8_2x3.npy", np.arange(6, dtype="<f8").reshape(2, 3) * 1.5)` |
| `i2_big_endian.npy` | `>i2` [-3, -2, -1, 0, 1, 2, 32767, -32768] | `np.save(..., np.array([...], dtype=">i2"))` |
| `u1_fortran.npy` | uint8 arange(12).reshape(3, 4), Fortran order | `np.save(..., np.asfortranarray(...))` |
| `c8.npy` | complex64 [1+2j, -0.5-0.25j, 3j] | `np.save` |
| `f2.npy` | float16 [0.5, -2, 65504, 6.1e-05, 0] | `np.save` |
| `bool.npy` | [[True, False], [False, True]] | `np.save` |
| `unicode.npy` | `<U5` ["alpha", "βeta", ""] | `np.save` |
| `bytes.npy` | `S3` [b"ab", b"cde"] | `np.save` |
| `record_aligned.npy` | aligned record dtype (id `<i4`, x `<f8`, name `S4`) with padding fields: (1, 2.5, "one"), (-2, -0.125, "two") | `np.save(..., np.array([...], dtype=np.dtype([...], align=True)))` |
| `i8_3d.npy` | int64 arange(24).reshape(2, 3, 4) - 12 | `np.save` |
| `v3_f4.npy` | format 3.0 header, float32 [1.25, -7.5] | `np.lib.format.write_array(f, arr, version=(3, 0))` |
| `v2_u4.npy` | format 2.0 header, uint32 [7, 4294967295] | `np.lib.format.write_array(f, arr, version=(2, 0))` |
| `arrays.npz` | `grid` = [[0, 1], [2, 3]] float64, `labels` = ["x", "y"] (stored) | `np.savez("arrays.npz", grid=..., labels=...)` |
| `arrays_compressed.npz` | the same, deflated | `np.savez_compressed(...)` |

## MATLAB (`gen_mat.py`, SciPy `savemat`)

`v5_uncompressed.mat` (`do_compression=False`) and `v7_compressed.mat`
(`do_compression=True`) hold: `matrix` (2x3 float64 0..2.5 step 0.5), `ints`
(int16 [-1 2 -3]), `big` (uint64 2^62+1), `flags` (logical [1 0 1]), `z`
(complex [1+2i 3-4i]), `label` ('synthetic text'), `lines` (char ['ab'; 'cd']),
`cells` (1x3 cell {1.0, 'two', uint8 [3 4]}), `config` (struct gain=2.5,
name='probe', inner.k=int32 [1 2]) and `sp` (sparse 2x2 with 2.0 at (2,1) and
1.5 at (1,2)). `v4.mat` (`format="4"`) holds `m` = [1 2; 3 4] and `s` = 5.5.
The creation date in the 116-byte text header of the two level-5 files was
replaced by a fixed `Thu Jan  1 00:00:00 1970` of the same length
(`scrub_mat_header.py`); nothing else was changed.

## NetCDF (`gen_netcdf.py`, netCDF4-python)

`classic.nc` (`NETCDF3_CLASSIC`), `offset64.nc` (`NETCDF3_64BIT_OFFSET`) and
`cdf5.nc` (`NETCDF3_64BIT_DATA`): global attributes title = "synthetic grid",
version = 3; dimensions time (unlimited, 2 records), y = 2, x = 3, nchar = 4;
variables `time` (f8, units "s", [0, 60]), `temp` (f4 [time, y, x],
arange(12) + 0.5, units "K", factors [1, 2]), `grid` (i2 [[1, -2, 3],
[-4, 5, -6]]), `names` (char [y, nchar] "ab", "wxyz"); `cdf5.nc` also has
`counts` (u8 [1, 2^40, 2^63]).

## FITS (`gen_fits.py`, astropy)

| File | Contents |
| --- | --- |
| `multi.fits` | primary uint16 6x8 image value = 1000 x + 100 y (BZERO 32768), OBJECT/EXPTIME/COMMENT cards; IMAGE `CUBE` float32 2x3x4 (arange - 5)/2; IMAGE `SCALED` int16 [[0,1],[2,3]] with BSCALE 0.5, BZERO 10; BINTABLE `CATALOG` (ID J, FLUX D, LABEL 6A, FLAG L, VEC 2E) 3 rows; ASCII TABLE `ASCII` (N I5, X F8.3, S A4) 2 rows |
| `gray8.fits` | primary uint8 [[0, 64, 128], [192, 255, 32]] |

Written with `HDUList([...]).writeto("multi.fits")` and
`PrimaryHDU(...).writeto("gray8.fits")`.

## Volumes (`gen_volumes.py`)

The volume v[x, y, z] = x + 10 y + 100 z for x < 4, y < 3, z < 2 (int16, x fastest):

| File | Tool | Command |
| --- | --- | --- |
| `vol.nii`, `vol.nii.gz` | nibabel | `nib.save(nib.Nifti1Image(vol, np.diag([0.5, 0.5, 2, 1])), ...)` with descrip "synthetic" |
| `scaled.nii` | nibabel | uint8 copy with `set_slope_inter(0.5, -1.0)` |
| `analyze.hdr` | nibabel | `nib.save(nib.AnalyzeImage(vol, np.eye(4)), "analyze.hdr")` (the `.img` half is not committed) |
| `vol_gzip.nrrd`, `vol_ascii.nrrd` | pynrrd | `nrrd.write(name, vol, header={"encoding": "gzip" / "ascii"})` |
| `vol_raw.nrrd` | pynrrd | float32 copy, `encoding: raw` |
| `vol.mha`, `vol_zlib.mha` | SimpleITK | `sitk.WriteImage(img, name, useCompression=False/True)`, spacing (0.5, 0.5, 2) |

pynrrd writes the creation time into a header comment; `scrub_volumes.py`
replaced it with `# on 1970-01-01 00:00:00(GMT).` (same length) and set
gzip member MTIME fields to 0.

## Interop of Convertible's output (checked while developing)

- `.npy`/`.npz` written by Convertible load with `numpy.load` (all dtypes above).
- MAT written from `v7_compressed.mat` loads with `scipy.io.loadmat`: every
  variable, including the cell, the nested struct and the sparse matrix.
- NetCDF written from `classic.nc` and `cdf5.nc` opens with netCDF4-python
  (`NETCDF3_CLASSIC` / `NETCDF3_64BIT_DATA`) with identical values and attributes.
- FITS written from `multi.fits` passes `astropy.io.fits` `verify("exception")`
  and gives the same images and tables.
- NIfTI (.nii, .nii.gz), NRRD and MHA written from `vol.nii` read back with
  nibabel, pynrrd and SimpleITK (voxel (3,2,1) = 123, int16).

## HDF5 (`gen_hdf5.py`, h5py 3.16.0 with HDF5 2.0.0)

`earliest.h5` (`libver="earliest"`: superblock v0, symbol-table groups,
v1 B-tree chunk index, layout v3) and `latest.h5` (`libver="latest"`:
superblock v3, v2 object headers, compact and dense link storage, chunk
indexes "single chunk" and "fixed array") both hold: root attributes title,
version (int32 2), scales [0.5, 1.5], note (variable-length string);
group `/grid` with attribute units = "K", `contiguous` (float64 2x3 =
arange(6)/4), `chunked` (int32 10x10 = arange(100)-50, chunks 4x3, gzip +
shuffle + Fletcher-32), `big_endian` (`>i4` [1, -2, 3]), `sub/scalar`
(float32 2.5); `/labels` (S3 ["ab", "cde"]), `/names` (variable-length
strings ["alpha", "beta"]), `/records` (compound id i2, value f8, tag S2),
`/colours` (enum on u1 [0, 1, 1]). `earliest.h5` adds `/compact` (compact
layout, uint8 [7, 8, 9]); `latest.h5` adds group `/many` with 20 datasets
`d00`..`d19` (int16 [i]) and 12 attributes `a00`..`a11` (dense storage),
`/single_chunk` (float32 3x4, one gzip chunk) and `/fixed_array` (uint16 5x6,
chunks 2x2). `cycle.h5` has `/a/x` and a hard link `/a/back` to the root.
`mat73.mat` is an HDF5 file with a 512-byte user block holding a MATLAB 7.3
style text header (written by the script itself) and dataset `m` = [[1, 2], [3, 4]].

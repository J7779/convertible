# Sources: science/chem

Small synthetic molecules and toy crystal cells for the chemistry tests.
Commands run in this directory. Tool versions: RDKit 2026.09.2 and ASE 3.29.0
in a Python 3 virtual environment; Open Babel 3.1.1 (Ubuntu 24.04 package)
in a throwaway Docker container.

| File | Contents | Tool | Command |
| --- | --- | --- | --- |
| `three.sdf` | ethanol (9 atoms), acetate (7 atoms, O4 charge -1, C=O double bond, data items `test_value` = `42`, `note` = `made-up property`), benzene (12 atoms, Kekulé bonds) with 3D coordinates | RDKit `SDWriter` | `gen_chem.py` |
| `ethanol.mol` | ethanol, V2000 | RDKit `MolToMolBlock` | `gen_chem.py` |
| `acetate_v3000.mol` | acetate, V3000 (`CHG=-1`) | RDKit `MolToV3KMolBlock` | `gen_chem.py` |
| `ethanol.xyz` | ethanol, coordinates with 6 decimals (the reference values of the tests) | RDKit `MolToXYZBlock` | `gen_chem.py` |
| `peptide.pdb` | Gly-Ala-Gly tripeptide, 14 heavy atoms, chain A, residues 1-3, CONECT records | RDKit `MolFromSequence("GAG")`, `EmbedMolecule(randomSeed=11)`, `MolToPDBBlock` | `gen_chem.py` |
| `ethanol.mol2` | ethanol with SYBYL types and Gasteiger charges | Open Babel 3.1.1 | `obabel ethanol.mol -O ethanol.mol2` |
| `ethanol.cml` | ethanol as CML with `hydrogenCount` | Open Babel 3.1.1 | `obabel ethanol.mol -O ethanol.cml -xp` |
| `peptide.cif` | the tripeptide as PDBx/mmCIF (`_atom_site` loop) | Open Babel 3.1.1 | `obabel peptide.pdb -O peptide.cif -ommcif` |
| `nacl.cif` | rock-salt NaCl primitive cell (a = 5.64), 2 sites | ASE `ase.io.write` | `gen_cif.py` |
| `monoclinic.cif` | toy monoclinic cell a=4, b=5, c=6, beta=100, C at (0.1,0.2,0.3), O at (0.4,0.5,0.6) fractional; ASE reports Cartesian C (0.087433, 1, 1.772654), O (0.974867, 2.5, 3.545308) | ASE `ase.io.write` | `gen_cif.py` |

`gen_chem.py`: each molecule is `Chem.AddHs(Chem.MolFromSmiles(smiles))`
embedded with `AllChem.EmbedMolecule(m, randomSeed=7)` and named with
`SetProp("_Name", ...)`; SMILES `CCO` (ethanol), `CC(=O)[O-]` (acetate,
plus the two data items), `c1ccccc1` (benzene).

`gen_cif.py`: `write("nacl.cif", bulk("NaCl", "rocksalt", a=5.64))` and
`write("monoclinic.cif", Atoms("CO", scaled_positions=[(0.1,0.2,0.3),(0.4,0.5,0.6)], cell=[4,5,6,90,100,90], pbc=True))`.

## Spectra

| File | Contents | Tool | Command |
| --- | --- | --- | --- |
| `band_affn.jdx` | JCAMP-DX AFFN (X++(Y..Y)), 25 points x = 400..1600 step 50 1/CM, y = round(100 exp(-((x-1000)/200)^2), 4) | `jcamp` 1.3.2 (`pip install jcamp`) | `jcamp.writefile("band_affn.jdx", {"title": "synthetic band", "data type": "INFRARED SPECTRUM", "xunits": "1/CM", "yunits": "ABSORBANCE", "x": x, "y": y})` |
| `asdf_example.jdx` | JCAMP-DX 4.24 compressed (SQZ/DIF/DUP with Y check values): 14 points x = 0..13, ordinates 10,20,30,30,30,25,20,20,20,20,15,-5,-5,0 times YFACTOR 0.5 | written by hand from the JCAMP-DX 4.24 rules; decoded by `jcamp.readfile` to exactly y = 5,10,15,15,15,12.5,10,10,10,10,7.5,-2.5,-2.5,0 | data lines `0A0J0T%TnT`, `6B0%Unk0%`, `12eN` |
| `run.mzML` | indexed mzML, 2 spectra: MS1 at 90 s (m/z 100.5, 200.25, 300.125, 400.0625; intensity 1000, 2500.5, 50.25, 7), MS2 at 105 s (m/z 150, 151.5; intensity 10, 20); zlib, 64-bit m/z, 32-bit intensity | pyOpenMS 3.6.0 `MzMLFile().store` with `setCompression(True)`, `setMz32Bit(False)`, `setIntensity32Bit(True)` | `gen_spectra.py` |
| `run.mzXML` | the same two scans as mzXML 3.1 (32-bit network-order pairs) | pyOpenMS 3.6.0 `MzXMLFile().store` | `gen_spectra.py` |

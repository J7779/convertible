# Sources: science/bio

Synthetic sequences, alignments, trees, feature tables and structures for the
bioinformatics tests. No real organism data: every sequence is a made-up
repeat or a short invented peptide.

Tools: Biopython 1.88 in a Python 3 virtual environment (`pip install biopython`).
The generator scripts are listed inline; run them in this directory.

## Sequences

| File | Contents | Tool | Command |
| --- | --- | --- | --- |
| `three.fasta` | 3 records: `seq1` (`ACGTACGTAC`x7 + `GGCC`, description "synthetic test sequence one"), `seq2` (`TTTTGGGGCCCCAAAA`), `prot1` (`MKTAYIAKQRQISFVKSHFSRQ`), 60 residues per line | Biopython `SeqIO.write(..., "fasta")` | `gen_seq.py` below |
| `reads_sanger.fastq` | 2 reads, `read1` `ACGTNACGTA` Phred 40,39,30,20,2,10,35,38,41,0; `read2` `GGGTTTAAAC` Phred 5..14 | `SeqIO.write(..., "fastq")` | `gen_seq.py` |
| `reads_illumina13.fastq` | same reads, Phred+64 | `SeqIO.write(..., "fastq-illumina")` | `gen_seq.py` |
| `reads_solexa.fastq` | same reads, Solexa+64 scores | `SeqIO.write(..., "fastq-solexa")` | `gen_seq.py` |
| `construct.gb` | GenBank record `TEST0001.1`, 95 bp circular DNA (`ATGGCTAAACGTTGA` + `ACGT`x20), features source / CDS (translation `MAKR`, wrapped note) / complement misc_feature | `SeqIO.write(..., "genbank")` | `gen_seq.py` |
| `construct.embl` | the same record as EMBL | `SeqIO.write(..., "embl")` | `gen_seq.py` |
| `test_protein.swiss` | invented UniProt entry `TEST1_SYNTH` / `Q00000`, 30 aa, 2 features | written by hand in a text editor (Biopython cannot write this format); checked with `SeqIO.read("test_protein.swiss", "swiss")`, which reports id Q00000, 30 aa and features CHAIN [0:30], DOMAIN [4:20] | - |

`gen_seq.py`:

```python
from Bio.Seq import Seq
from Bio.SeqRecord import SeqRecord
from Bio.SeqFeature import SeqFeature, FeatureLocation
from Bio import SeqIO
recs = [SeqRecord(Seq("ACGTACGTAC"*7 + "GGCC"), id="seq1", description="synthetic test sequence one"),
        SeqRecord(Seq("TTTTGGGGCCCCAAAA"), id="seq2", description="second"),
        SeqRecord(Seq("MKTAYIAKQRQISFVKSHFSRQ"), id="prot1", description="made-up peptide")]
SeqIO.write(recs, "three.fasta", "fasta")
q = [SeqRecord(Seq("ACGTNACGTA"), id="read1", description="lane=1"),
     SeqRecord(Seq("GGGTTTAAAC"), id="read2", description="")]
q[0].letter_annotations["phred_quality"] = [40,39,30,20,2,10,35,38,41,0]
q[1].letter_annotations["phred_quality"] = [5,6,7,8,9,10,11,12,13,14]
SeqIO.write(q, "reads_sanger.fastq", "fastq")
SeqIO.write(q, "reads_illumina13.fastq", "fastq-illumina")
SeqIO.write(q, "reads_solexa.fastq", "fastq-solexa")
r = SeqRecord(Seq("ATGGCTAAACGTTGA" + "ACGT"*20), id="TEST0001.1", name="TEST0001",
              description="Synthetic test construct for parser checks")
r.annotations.update(molecule_type="DNA", topology="circular", data_file_division="SYN", date="01-JAN-2000",
                     organism="synthetic construct", source="synthetic construct",
                     taxonomy=["other sequences", "artificial sequences"], accessions=["TEST0001"],
                     keywords=["test", "parser"])
r.features = [
  SeqFeature(FeatureLocation(0, 95, strand=1), type="source",
             qualifiers={"organism": ["synthetic construct"], "mol_type": ["other DNA"]}),
  SeqFeature(FeatureLocation(0, 15, strand=1), type="CDS",
             qualifiers={"gene": ["tstA"], "codon_start": ["1"], "translation": ["MAKR"],
                         "note": ["a long note that will need to wrap across more than one line in the flat file output format"]}),
  SeqFeature(FeatureLocation(20, 40, strand=-1), type="misc_feature", qualifiers={"label": ["reverse bit"]})]
SeqIO.write([r], "construct.gb", "genbank")
SeqIO.write([r], "construct.embl", "embl")
```

## Feature tables and alignments

| File | Contents | Tool | Command |
| --- | --- | --- | --- |
| `annotation.gff3` | 6 GFF3 features of an invented gene `gene1`/`tstA` on `chrT` (gene, mRNA, 2 exons, 2 CDS with phases 0 and 2) | gffread 0.12.7 (Ubuntu 24.04 package, in a throwaway Docker container) | `gffread in.gff3 -F -O -o annotation.gff3` |
| `annotation.gtf` | the same model as GTF (transcript, exons, CDS with `transcript_id`/`gene_id`) | gffread 0.12.7 | `gffread in.gff3 -T -o annotation.gtf` |
| `intervals.bed` | 3 BED6 intervals `peakA` chrT:10-20 (+, 900), `peakB` chrT:500-600 (-, 300), `peakC` chrU:0-5 | bedtools 2.31.1 | `bedtools sort -i in.bed > intervals.bed` |
| `aligned.sam` | header (@HD, 2 @SQ) and 2 reads: `r001` flag 99 chrT:100 MAPQ 60 `8M2I` with tags NM:i, XS:Z, XF:f, XB:B:C; `r002` unmapped | pysam 0.24.1 `AlignmentFile(..., "w")` | `gen_align.py` |
| `aligned.bam` | the same records as BGZF-compressed BAM | pysam 0.24.1 `AlignmentFile(..., "wb")` | `gen_align.py` |
| `variants.vcf` | VCF 4.2, 2 variants on chrT with samples `sampleA`/`sampleB` (GT), INFO DP | pysam 0.24.1 `VariantFile(..., "w")` | `gen_align.py` |

`in.gff3` and `in.bed` are short hand-written inputs (the same records as above,
with IDs on every feature and the BED lines unsorted); the committed files are
what gffread and bedtools wrote from them.

`gen_align.py` builds the two reads and two variants with pysam's object API
(`AlignedSegment`, `VariantHeader`, `new_record`) using exactly the values in
the table above and writes them with `AlignmentFile`/`VariantFile`.

## Multiple alignments and trees

Three aligned 70-column sequences `taxonA` (`ACGTTGCAAC`x7), `taxonB` (gap at
column 6, `G` at column 41) and `taxonC` (last base `A`), written with
Biopython 1.88 `AlignIO.write`, and one tree written with `Phylo.write`
(`gen_msa.py`):

| File | Contents | Command |
| --- | --- | --- |
| `msa.phy` | strict interleaved PHYLIP | `AlignIO.write(aln, "msa.phy", "phylip")` |
| `msa_sequential.phy` | strict sequential PHYLIP | `AlignIO.write(aln, "msa_sequential.phy", "phylip-sequential")` |
| `msa_relaxed.phy` | relaxed PHYLIP, names `a_rather_long_name_1`, `a_rather_long_name_2`, `short` | `AlignIO.write(long, "msa_relaxed.phy", "phylip-relaxed")` |
| `msa.aln` | Clustal | `AlignIO.write(aln, "msa.aln", "clustal")` |
| `msa.sto` | Stockholm | `AlignIO.write(aln, "msa.sto", "stockholm")` |
| `msa.nex` | NEXUS DATA block | `AlignIO.write(aln, "msa.nex", "nexus")` |
| `tree.nwk` | Newick `((taxonA:0.1,taxonB:0.25)0.95:0.05,'taxon C':0.3,(D:0.001,E:2)inner:0.4):0;` | `Phylo.write(tree, "tree.nwk", "newick")` |
| `tree.nex` | the same tree as a NEXUS TAXA + TREES file | `Phylo.write(tree, "tree.nex", "nexus")` |

The tree was read with `Phylo.read(io.StringIO("((taxonA:0.1,taxonB:0.25)0.95:0.05,'taxon C':0.3,(D:1e-3,E:2)inner:0.4);"), "newick")`.

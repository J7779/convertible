#!/bin/sh
# Google woff2_compress + mkeot. Runs in container, cwd /w.
set -e
mkdir -p tmp && cd tmp
for f in inter_subset.ttf pair.ttc tiny_variable.ttf; do cp ../out/$f .; woff2_compress $f; done
cp inter_subset.woff2 pair.woff2 tiny_variable.woff2 ../out/
mkeot ../out/inter_subset.ttf > ../out/inter_subset.eot
cd ..
ls -l out/*.woff2 out/*.eot
for f in out/inter_subset.woff2 out/pair.woff2 out/tiny_variable.woff2; do printf "%s: " $f; ots-sanitize $f >/dev/null && echo OK; done
python3 - <<'EOF'
import struct
d=open('out/inter_subset.eot','rb').read()
EOTSize,FontDataSize,Version,Flags=struct.unpack('<IIII',d[:16])
print('EOT size',EOTSize,len(d),'fontdata',FontDataSize,'version',hex(Version),'flags',hex(Flags))
print(d[82:400])
EOF

#!/bin/sh
# Uploads to the console's ftpd (docs/PLAN.md): the small app (built with `make ROMFS=`, ~0.5 MB) and, with
# "data", the baked data once (romfs/ -> sdmc:/3ds/mwcell3d/data/, ~180 MB; again only when the bake changes).
#   sh tools/upload.sh            the app + scripts
#   sh tools/upload.sh data       the data first (index.bin last: the game takes the SD data once it is there)
cd "$(dirname "$0")/.."
FTP=${FTP:-ftp://192.168.1.90:5000/3ds/mwcell3d}
put() { curl -s --ftp-create-dirs -T "$1" "$FTP/$2" || { echo "FAILED $1"; exit 1; }; }
if [ "$1" = data ]; then
    find romfs -type f ! -name index.bin | while read -r f; do put "$f" "data/${f#romfs/}"; done
    put romfs/index.bin data/index.bin
    echo "data: done"
fi
for f in scripts/*.cs; do put "$f" "scripts/$(basename "$f")"; done
put mwcell3d.3dsx mwcell3d.3dsx
echo "app: $(wc -c < mwcell3d.3dsx) bytes"

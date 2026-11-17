#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "Usage: bash tools/mksysroot.sh 'Q2 Firmware V1.36.zip' SYSROOT" >&2
    exit 2
fi
stock=$1
sysroot=$2
expected=4e298cab6c12d86cfc55565645f7e3ae11d3fb44677a93ca13991e7184ecec4e
if ! sha256sum --check --status <<< "$expected  $stock"; then
    echo "Use the original Shanling Q2 Firmware V1.36 ZIP." >&2
    exit 1
fi
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

# Debian's glibc 2.28 development headers match the Q2's userspace.
while read -r checksum package; do
    archive="$scratch/${package##*/}"
    curl -fsSL --retry 2 "https://archive.debian.org/debian/pool/main/$package" -o "$archive"
    sha256sum --check --status <<< "$checksum  $archive"
    ar p "$archive" data.tar.xz | tar -xJf - -C "$scratch" ./usr/include
done <<'PACKAGES'
e28bac8637e98d78045f0fd05613ed2ab298fb1aa6387633b6d609119774d590 g/glibc/libc6-dev_2.28-10+deb10u1_mipsel.deb
5f6ca1c34bb326b38fa44838b1ed051c33a2591bf7bfa26276e54fc85c00427c l/linux/linux-libc-dev_4.19.249-2_mipsel.deb
PACKAGES

gnu="$scratch/usr/include/mipsel-linux-gnu/gnu"
cp "$gnu/stubs-o32_hard.h" "$gnu/stubs-o32_hard_2008.h"
unzip -p "$stock" 'Q2 Firmware V1.36/update.tar' |
    tar -xOf - recovery-update/rootfs.squashfs > "$scratch/rootfs.squashfs"
unsquashfs -processors 1 -no-progress -d "$scratch/root" "$scratch/rootfs.squashfs" lib > /dev/null

mkdir -p "$sysroot/usr/include" "$sysroot/lib"
cp -R "$scratch/usr/include/." "$sysroot/usr/include/"
for lib in libc.so.6 libgcc_s.so.1 ld-linux-mipsn8.so.1; do
    cp -L "$scratch/root/lib/$lib" "$sysroot/lib/$lib"
done
echo "Q2 sysroot is ready at $sysroot"

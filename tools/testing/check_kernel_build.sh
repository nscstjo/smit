#!/usr/bin/env bash
# Compile-only: never loads a module or contacts the USB device.
set -euo pipefail
if [[ $# != 3 ]]; then
    echo "Usage: $0 KDIR DVB_USB_V2_DIR NEW_OUTPUT_DIRECTORY" >&2
    exit 2
fi
project=$(cd "$(dirname "$0")/../.." && pwd)
kdir=$(realpath "$1")
dvb=$(realpath "$2")
out=$3
[[ -r "$kdir/Module.symvers" && -r "$dvb/dvb_usb.h" ]]
[[ ! -e "$out" ]] || { echo "Output directory must not exist" >&2; exit 2; }
mkdir -p "$out/driver" "$out/include"
out=$(realpath "$out")
cp "$project"/driver/*.[ch] "$project/driver/Makefile" "$out/driver/"
cp "$project"/include/*.h "$out/include/"
exec > >(tee "$out/build.log") 2>&1
# Old kernel-doc can fail with modern Perl; empty explicitly selects baseline.
make -C "$out/driver" KDIR="$kdir" DVB_USB_V2_DIR="$dvb" W="${SMIT_KBUILD_WARNINGS-1}"
sha256sum "$out"/driver/*.[ch] "$out"/driver/*.ko > "$out/sha256.txt"
modinfo "$out/driver/smit.ko"

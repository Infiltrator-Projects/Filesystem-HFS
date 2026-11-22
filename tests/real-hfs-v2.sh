#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Independently formatted HFS (not hand-crafted fixture only), read-only host.
set -euo pipefail
test_app=$(realpath "${1:?test executable}")
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/home"
export HOME="$work/home"
image="$work/hfs.img"
truncate -s 64M "$image"
hformat -l NativeHFS "$image"
head -c 8388608 /dev/urandom > "$work/kernel-payload"
head -c 2097152 /dev/urandom > "$work/initrd-payload"
hcopy -r "$work/kernel-payload" :kernel.img
hcopy -r "$work/initrd-payload" :initramfs
sha256sum "$image" > "$work/original.sha256"
"$test_app" "$image" "$work/kernel-payload" "$work/initrd-payload"
(cd "$work" && sha256sum -c original.sha256)
cp "$image" "$work/invalid.img"
printf '\000\000' | dd of="$work/invalid.img" bs=1 seek=1024 conv=notrunc status=none
"$test_app" "$work/invalid.img" "$work/kernel-payload" "$work/initrd-payload" --reject
# Use the native MDB fields to find the first catalog leaf. Corrupt only
# its leading key length, leaving the valid superblock and allocation bitmap.
read -r a b < <(od -An -tu1 -j 1052 -N2 "$image")
alloc_start=$(((a*256+b)*512))
read -r a b c d < <(od -An -tu1 -j 1044 -N4 "$image")
alloc_bytes=$(((a<<24)+(b<<16)+(c<<8)+d))
read -r a b < <(od -An -tu1 -j 1174 -N2 "$image")
catalogue=$((alloc_start+(a*256+b)*alloc_bytes))
read -r a b c d < <(od -An -tu1 -j $((catalogue+24)) -N4 "$image")
first_leaf=$(((a<<24)+(b<<16)+(c<<8)+d))
read -r a b < <(od -An -tu1 -j $((catalogue+32)) -N2 "$image")
node_bytes=$((a*256+b))
test "$first_leaf" -gt 0
test "$node_bytes" -ge 512
test "$node_bytes" -le 32768
# The catalogue header is a separate corruption class: the formatted MDB
# and allocation bitmap remain valid, so the reader must release its host
# bitmap on the failed Common V2 MOUNT. Use a separate expendable image.
cp --reflink=auto "$image" "$work/invalid-header.img"
printf '\\000' | dd of="$work/invalid-header.img" bs=1 \\
  seek=$((catalogue+8)) conv=notrunc status=none
sha256sum "$work/invalid-header.img" > "$work/header.sha256"
"$test_app" "$work/invalid-header.img" "$work/kernel-payload" \\
    "$work/initrd-payload" --corrupt-header
(cd "$work" && sha256sum -c header.sha256)
cp "$image" "$work/invalid-catalog.img"
printf '\377' | dd of="$work/invalid-catalog.img" bs=1 \
  seek=$((catalogue+first_leaf*node_bytes+14)) conv=notrunc status=none
"$test_app" "$work/invalid-catalog.img" "$work/kernel-payload" \
    "$work/initrd-payload" --corrupt-catalog
(cd "$work" && sha256sum -c original.sha256)
echo HFS_COMMON_V2_EXTERNAL_FORMAT_AND_READ_ONLY_NONMUTATION_PASS

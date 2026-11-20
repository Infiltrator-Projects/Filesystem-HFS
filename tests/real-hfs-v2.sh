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
(cd "$work" && sha256sum -c original.sha256)
echo HFS_COMMON_V2_EXTERNAL_FORMAT_AND_READ_ONLY_NONMUTATION_PASS

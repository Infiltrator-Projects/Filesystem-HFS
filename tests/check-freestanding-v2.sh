#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Check the actual Common V2 library and its embedded freestanding ELF.
set -euo pipefail
module=$(realpath "${1:?HFS.so required}")
test -s "$module"
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bash "$root/tests/check_v2_exports.sh" "$module"
readelf -SW "$module" | grep -F .infiltrator_boot >/dev/null
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
objcopy --dump-section ".infiltrator_boot=$work/reader.so" "$module" "$work/carrier-copy.so"
test -s "$work/reader.so"
readelf -hW "$work/reader.so" | grep -F 'DYN (Shared object file)' >/dev/null
! readelf -dW "$work/reader.so" | grep -F '(NEEDED)'
! nm -u "$work/reader.so" | grep .
nm -D --defined-only "$work/reader.so" | grep -F infiltrator_filesystem_api_v2 >/dev/null
if readelf -Wr "$work/reader.so" | grep -E 'R_X86_64_' | grep -v 'R_X86_64_RELATIVE'; then
  echo 'Unsupported freestanding ELF relocation' >&2
  exit 1
fi
echo HFS_COMMON_V2_CARRIER_AND_EMBEDDED_READER_PASS

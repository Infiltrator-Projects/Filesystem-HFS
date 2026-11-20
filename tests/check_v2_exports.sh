#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
file=${1:?HFS.so required}
test -s "$file"
test "$(nm -D --defined-only "$file" | awk '$3=="infiltrator_filesystem_api_v2"{n++} END{print n+0}')" = 1
unexpected="$(nm -D --defined-only "$file" | awk '{print $3}' |
  sed -E 's/@@?.*$//' |
  grep -v '^infiltrator_filesystem_api_v2$' || true)"
if test -n "$unexpected"; then
  printf 'Non-V2 exports leaked: %s\n' "$unexpected" >&2
  exit 1
fi
if nm -a "$file" |
   grep -E 'ifs_bind_services|ifs_unbind_services|ifs_hfs_worker_main|infiltrator_filesystem_.*_v1'; then
   echo 'Legacy runtime callbacks leaked into provider' >&2
   exit 1
fi

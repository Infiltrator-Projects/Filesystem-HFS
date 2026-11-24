#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# An independent HFS implementation must mount, create and read back
# a file on the volume produced by the actual packaged Common V2 module.
set -eu
testbin=${1:?native format test required}
module=${2:?production SO required}
oracle=${3:-}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/home"
export HOME="$work/home"
"$testbin" "$module" "$work/hfs-512.img" 1 512
"$testbin" "$module" "$work/hfs-4096.img" 1 4096
if [ -n "$oracle" ]; then
    printf 'Independent HFS catalogue and allocation validation\n' > "$work/payload"
    for image in "$work/hfs-512.img" "$work/hfs-4096.img"; do
        hmount "$image"
        hls -a :
        hcopy -r "$work/payload" :probe.txt
        hcopy -r :probe.txt "$work/copied"
        cmp "$work/payload" "$work/copied"
        hdel :probe.txt
        humount
        rm "$work/copied"
    done
fi

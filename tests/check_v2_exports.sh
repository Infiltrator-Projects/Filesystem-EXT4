#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
module=${1:?EXT4.so required}
symbols=$(readelf -W --dyn-syms "$module" | awk '$4 == "FUNC" && $7 != "UND" && $8 ~ /^infiltrator_filesystem_/ {print $8}')
if [ "$symbols" != 'infiltrator_filesystem_api_v2' ]; then
    printf 'Expected only Common V2 export, found: %s\n' "$symbols" >&2
    exit 1
fi
readelf -p .ifsmeta "$module" | grep -Fq 'IFS-MODULE:ext4:common2' ||
    { echo 'Missing verified Common V2 EXT4 identity' >&2; exit 1; }

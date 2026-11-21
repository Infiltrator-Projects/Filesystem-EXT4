#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Verify the installed EXT4.so carries the identical, independently
# executable Common V2 firmware reader. No BOOT validator is emulated.
set -euo pipefail
carrier=${1:?installed EXT4.so carrier required}
firmware=${2:?freestanding EXT4.so required}
test -s "$carrier" && test -s "$firmware"
readelf -h "$firmware" | grep -Eq 'Type:.*DYN'
readelf -h "$firmware" | grep -Eq 'Machine:.*X86-64'
nm -D --defined-only "$firmware" | grep -Eq '[[:space:]]infiltrator_filesystem_api_v2$'
nm -D --defined-only "$carrier" | grep -Eq '[[:space:]]infiltrator_filesystem_api_v2$'
if nm -D --undefined-only "$firmware" | grep -q '[^[:space:]]'; then
    echo 'Firmware profile has unresolved runtime imports' >&2
    nm -D --undefined-only "$firmware" >&2
    exit 1
fi
if readelf -d "$firmware" | grep -Fq '(NEEDED)'; then
    echo 'Firmware profile imports a hosted shared library' >&2
    exit 1
fi
if readelf -l "$firmware" | grep -Fq 'INTERP'; then
    echo 'Firmware profile requires a hosted dynamic interpreter' >&2
    exit 1
fi
if nm -D --defined-only "$firmware" | grep -Eq 'infiltrator_filesystem_.*_v1'; then
    echo 'Retired filesystem ABI exported by firmware profile' >&2
    exit 1
fi
readelf -SW "$carrier" | grep -Fq '.infiltrator_boot'
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
objcopy --dump-section ".infiltrator_boot=$tmp/extracted.elf" "$carrier"
cmp "$firmware" "$tmp/extracted.elf"
echo EXT4_FREESTANDING_COMMON_V2_IDENTICAL_CARRIER_PASS

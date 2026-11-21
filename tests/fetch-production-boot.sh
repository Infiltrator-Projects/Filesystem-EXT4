#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Obtain BOOT's genuine published production validator and EFI image.
# No local reimplementation or weaker compatibility check is permitted.
set -euo pipefail
work=$(realpath -m "${1:?qualification directory required}")
mkdir -p "$work"
command -v gh >/dev/null || { echo 'gh is required' >&2; exit 2; }
command -v dpkg-deb >/dev/null || { echo 'dpkg-deb is required' >&2; exit 2; }
command -v sha256sum >/dev/null || { echo 'sha256sum is required' >&2; exit 2; }
: "${GH_TOKEN:?GitHub authentication is required for release selection}"
repo=Infiltrator-Projects/Boot
# A pinned tag can be supplied to reproduce an earlier exact qualification.
# The default is the newest published BOOT release, not an ancient refind
# checkout, removed Git ref, or deleted asset. Record which immutable release.
tag=${BOOT_VALIDATOR_TAG:-}
if [[ -z "$tag" ]]; then
    tag=$(gh api "repos/$repo/releases?per_page=1" --jq '.[0].tag_name')
fi
if [[ ! "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Cannot select a published BOOT release: $tag" >&2
    exit 1
fi
echo "Testing production BOOT artifact $repo $tag" >&2
gh release download "$tag" --repo "$repo" --dir "$work" \
    --pattern 'infiltrator-boot_*_amd64.deb' --pattern SHA256SUMS --clobber
shopt -s nullglob
packages=("$work"/infiltrator-boot_*_amd64.deb)
if (( ${#packages[@]} != 1 )) || [[ ! -s "$work/SHA256SUMS" ]]; then
    echo 'BOOT release must contain exactly one DEB and SHA256SUMS' >&2
    exit 1
fi
deb=${packages[0]}
file=$(basename "$deb")
# Verify the exact published package bytes against BOOT's own release digest.
line=$(grep -E "^[0-9a-fA-F]{64}  ${file}$" "$work/SHA256SUMS" || true)
if [[ $(printf '%s\n' "$line" | grep -c . || true) != 1 ]]; then
    echo "BOOT release SHA256SUMS lacks a unique checksum for $file" >&2
    exit 1
fi
(cd "$work" && printf '%s\n' "$line" | sha256sum -c -)
dpkg-deb -x "$deb" "$work/payload"
mapfile -t validators < <(find "$work/payload" -type f -name validate-module)
if (( ${#validators[@]} != 1 )) || [[ ! -x ${validators[0]:-} ]]; then
    echo "BLOCKED: BOOT $tag does not ship the executable production Common V2 validate-module." >&2
    echo 'Filesystem releases cannot claim BOOT compatibility or be published.' >&2
    exit 1
fi
# The image must come from the same authenticated BOOT package as validator.
mapfile -t images < <(find "$work/payload" -type f \
    \( -name BOOTX64.EFI -o -name boot_x64.efi \))
if (( ${#images[@]} != 1 )) || [[ ! -s ${images[0]:-} ]]; then
    echo "BLOCKED: BOOT $tag does not contain exactly one supported x64 EFI image." >&2
    exit 1
fi
printf '%s\n' "${validators[0]}" > "$work/validator.path"
printf '%s\n' "${images[0]}" > "$work/efi.path"
printf '%s\n' "$tag" > "$work/tag"
echo "Verified BOOT validator+EFI from $tag" >&2

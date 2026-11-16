#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Only a disposable regular file is formatted and read; never touch disks.
set -euo pipefail
program=${1:?path to test executable}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/tree/sub"
printf 'Common V2 EXT4 shared-object namespace\n' > "$work/tree/hello.txt"
printf 'inside the directory\n' > "$work/tree/sub/inside.txt"
ln -s /hello.txt "$work/tree/alias.txt"
truncate -s 64M "$work/disk.img"
features=has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,flex_bg
features+=,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum
mkfs.ext4 -q -F -b 4096 -O "none,$features" -L EXT4ROOT -d "$work/tree" "$work/disk.img"
before=$(sha256sum "$work/disk.img" | cut -d' ' -f1)
"$program" "$work/disk.img"
after=$(sha256sum "$work/disk.img" | cut -d' ' -f1)
test "$before" = "$after"

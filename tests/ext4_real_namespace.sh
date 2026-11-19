#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Only a disposable regular file is formatted and read; never touch disks.
set -euo pipefail
program=${1:?path to test executable}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/tree/sub" "$work/tree/etc"
printf 'Common V2 EXT4 shared-object namespace\n' > "$work/tree/hello.txt"
# A populated, checksum-protected HTree directory like Zabbix's /etc.
printf 'UUID=11111111-2222-3333-4444-555555555555 / ext4 defaults 0 1\n' > "$work/tree/etc/fstab"
# Approximately match Zabbix's 12 KiB directory: one HTree root and two
# leaves, with fstab in a normal leaf rather than an inline inode.
for ((n=0; n<170; ++n)); do
    printf 'entry-%04d\n' "$n" > "$(printf '%s/tree/etc/entry-%04d-abcdefghijklmno' "$work" "$n")"
done
printf 'inside the directory\n' > "$work/tree/sub/inside.txt"
ln -s /hello.txt "$work/tree/alias.txt"
truncate -s 64M "$work/disk.img"
features=has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,flex_bg
features+=,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum
mkfs.ext4 -q -F -b 4096 -O "none,$features" -L EXT4ROOT -d "$work/tree" "$work/disk.img"
# mkfs -d alone does not guarantee an indexed HTree. Force the independent
# e2fsprogs index rebuild and refuse to count an ordinary linear directory
# as an HTree regression. This operates ONLY on the disposable image.
fsck_status=0
e2fsck -f -y -D "$work/disk.img" >/dev/null 2>&1 || fsck_status=$?
case "$fsck_status" in 0|1) ;; *) echo "HTree fixture repair failed ($fsck_status)" >&2; exit 1 ;; esac
tree=$(debugfs -R 'htree_dump /etc' "$work/disk.img" 2>&1)
grep -Fq 'Root node dump:' <<< "$tree"
grep -Fq 'Number of entries (count): 2' <<< "$tree"
before=$(sha256sum "$work/disk.img" | cut -d' ' -f1)
"$program" "$work/disk.img"
after=$(sha256sum "$work/disk.img" | cut -d' ' -f1)
test "$before" = "$after"

# In a separate disposable copy, flip exactly one bit in the HTree root
# checksum (dx_tail, final four bytes of the 4 KiB index block). A provider
# that silently accepts corrupted index metadata must fail qualification.
# Verify the block mapping before any dd write; NEVER use a device path.
bad="$work/corrupt-htree.img"
cp -- "$work/disk.img" "$bad"
physical=$(debugfs -R 'bmap /etc 0' "$bad" 2>/dev/null | tr -d '[:space:]')
[[ "$physical" =~ ^[1-9][0-9]*$ ]] || {
    echo "Failed to resolve indexed /etc block in disposable image" >&2
    exit 1
}
checksum_byte=$((physical * 4096 + 4092))
old=$(dd if="$bad" bs=1 skip="$checksum_byte" count=1 status=none | od -An -tu1)
[[ "$old" =~ ^[[:space:]]*[0-9]+[[:space:]]*$ ]] || exit 1
printf -v replacement '%03o' $((old ^ 1))
printf '%b' "\\$replacement" | dd of="$bad" bs=1 seek="$checksum_byte" count=1 conv=notrunc status=none
"$program" "$bad" --corrupt-htree

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Boot a real Linux EFI stub via the V2-only EXT4 reader in disposable OVMF.
set -euo pipefail
boot=$(realpath "${1:?BOOTX64.EFI required}")
module=$(realpath "${2:?firmware EXT4.so required}")
kernel=$(realpath "${3:?Linux EFI stub image required}")
work=$(realpath -m "${4:?output directory required}")
ovmf=${OVMF_DIR:-/usr/share/OVMF}
mkdir -p "$work"
esp="$work/esp.img"
media="$work/ext4.img"
tree="$work/root"
ram="$work/ram"
mkdir -p "$tree/System/Kernel/Images" "$ram"/{bin,dev,proc}
images="$tree/System/Kernel/Images"
cp "$kernel" "$images/native.img"
cp "$(command -v busybox)" "$ram/bin/"
ln -sf busybox "$ram/bin/sh"
cat > "$ram/init" <<'GUEST'
#!/bin/sh
set -eu
b=/bin/busybox
$b mount -t devtmpfs devtmpfs /dev
exec </dev/ttyS0 >/dev/ttyS0 2>&1
trap '/bin/busybox poweroff -f' EXIT
$b mount -t proc proc /proc
$b grep -qw ext4_so_trial /proc/cmdline
echo EXT4_COMMON_V2_LINUX_HANDOFF_PASS
GUEST
chmod +x "$ram/init"
(cd "$ram" && find . -print0 | cpio --null -o -H newc --owner=0:0 --quiet | gzip -1) > "$images/native.initrd"
# Exercise absolute and relative links. These are ordinary EXT4 directory entries.
ln -sf /System/Kernel/Images/native.img "$tree/vmlinuz"
ln -sf native.initrd "$images/current.initrd"
truncate -s 128M "$media"
mkfs.ext4 -q -F -b 4096 -L EXT4ROOT -d "$tree" "$media"
before=$(sha256sum "$media")
truncate -s 64M "$esp"
mkfs.fat -F32 "$esp"
mmd -i "$esp" ::/EFI ::/EFI/BOOT ::/EFI/BOOT/filesystems_x64
mcopy -i "$esp" "$boot" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$esp" "$module" ::/EFI/BOOT/filesystems_x64/EXT4.so
cat > "$work/boot.conf" <<'BOOT'
timeout 1
textonly true
use_nvram false
driver_diagnostics true
scanfor manual
menuentry "InfiltratorOS kernel from EXT4.so" {
    volume EXT4ROOT
    filesystem_module ext4
    loader \vmlinuz
    initrd \System\Kernel\Images\current.initrd
    linux_load memory
    ostype Linux
    options "console=ttyS0 rdinit=/init panic=1 ext4_so_trial"
}
BOOT
mcopy -i "$esp" "$work/boot.conf" ::/EFI/BOOT/
cp "$ovmf/OVMF_VARS_4M.fd" "$work/vars.fd"
qemu=(qemu-system-x86_64 -machine q35,accel=tcg -cpu max -m 768 -display none
      -monitor none -net none -no-reboot -serial "file:$work/serial.log"
      -drive "if=pflash,format=raw,readonly=on,file=$ovmf/OVMF_CODE_4M.fd"
      -drive "if=pflash,format=raw,file=$work/vars.fd"
      -drive "file=$esp,format=raw,if=virtio"
      -drive "file=$media,format=raw,if=virtio,readonly=on")
if [[ -n ${QEMU_DATA_DIR:-} ]]; then
    qemu+=(-L "$QEMU_DATA_DIR")
fi
if ! timeout 120 "${qemu[@]}" ||
   ! grep -Fq 'EXT4_COMMON_V2_LINUX_HANDOFF_PASS' "$work/serial.log"; then
    tail -n 70 "$work/serial.log" >&2
    exit 1
fi
grep -Fq 'validated EXT4.so' "$work/serial.log"
grep -Fq 'close /vmlinuz:' "$work/serial.log"
grep -Fq 'close /System/Kernel/Images/current.initrd:' "$work/serial.log"
test "$(sha256sum "$media")" = "$before"
echo EXT4_COMMON_V2_LINUX_HANDOFF_PASS

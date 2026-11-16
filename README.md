# Filesystem-EXT4
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/EXT4.so`
- `/System/Resources/Documentation/Filesystem-EXT4.pdf`
- `/System/Kernel/Filesystems/ext4.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.

## BOOT qualification

`EXT4.so` carries a freestanding Common V2 ELF built from `module/firmware`.
BOOT 0.1.47 or later can read it from the same installed file used by
desktop applications. Qualification loads a kernel and initramfs through
that module on a GPT-partitioned NVMe image and checks the EXT4 partition.
The reader handles extent-backed files, sparse ranges, symbolic links and
metadata checksums on clean EXT4 volumes with 1, 2 or 4 KiB blocks. It
rejects volumes that require journal replay and layouts outside its supported
feature set. Automatic ESP deployment requires a qualified boot entry and
preservation of the existing recovery entry.

## Hosted Common V2 namespace (1.0.13)

The installed `EXT4.so` now implements read-only `CORE.STAT`,
`CORE.LIST` and `CORE.READ` using the identical format-reader source as the
firmware ELF. Both profiles use the canonical EXT4 core and bounded Common V2
host I/O; the hosted module supports multiple independent mount sessions.
The old Defragmenter CLI and V1 service adapter are absent from the installed
shared object. Tests use disposable real EXT4 images as well as OVMF kernel
handoff. This does not yet register a Linux VFS mount or support writable
root mounting; the generic kernel-side Common V2 loader remains required.

## Native file-mode and ownership metadata (Common V2)

The EXT4.so portable engine exports `METADATA.GET_MODE` using the verified
inode permission and owner/group fields. InfiltratorOS Kernel can now present
real executable bits and ownership through the generic V2 metadata group,
without introducing EXT4-specific code into the Kernel. Timestamp reporting,
metadata mutation and writable mounting remain separate qualifications.

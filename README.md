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

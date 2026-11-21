# EXT4 local source consolidation — 6 November 2026

Every one of the 40 tracked files in the central Filesystems EXT4 native source tree at `a253fb724ce1fd760a361219281effb6abd71367` is present locally in this independent repository. Its 3 existing canonical core/design files were byte-identical, so preserved; the complete Linux VFS and embedded journal source was imported verbatim with author/license notices unchanged.

The independent EXT4 shared-object implementation and its qualification tests remain in this Git. The `standalone/kernel.json` builds `ext4.ko` from this repository's local `native/filesystems/ext4/` tree, not a pinned download of central Filesystems. Only shared Libraries/Common code remains an external source dependency for the native `.so` build.

Kernel build/runtime, journal recovery, media and root/EFI qualification remain separate acceptance gates. Do not delete the central EXT4 implementation until the legacy Filesystems CMake, CI and packaging consumers have switched and pass independent post-change checks.

## Strict Common V2 read-only EXT4 volume lifecycle (1.0.9)
CORE.PROBE, CORE.MOUNT/UNMOUNT and VOLUME.QUERY now use canonical
EXT4 geometry and feature validation with partition-relative Common V2
host reads. The probe checks high 64-bit block-count fields whenever
EXT4 64BIT is set and refuses media-exceeding or invalid free-block counts.
Free space is the **recorded superblock count**, not a scanned bitmap;
transaction, namespace mutation, journal recovery and DEFRAG operations
remain in their original independent engine and are not advertised as
V2 MAP/PLAN/EXECUTE/VERIFY/RECOVER until actually qualified.

## Hosted Common V2 namespace, sourced from the firmware reader (1.0.13)

The installed `EXT4.so` uses the **same source-owned portable reader** as
BOOT's freestanding ELF (`module/firmware/ext4.c`) with distinct hosted and
firmware compilation profiles. The hosted Common V2 descriptor now supports
`CORE.PROBE / MOUNT / UNMOUNT / STAT / LIST / READ` and
`VOLUME.QUERY / LABEL_GET` on validated clean EXT4 images. The
`.infiltrator_boot` payload still contains the independent freestanding
ELF, not hosted glibc/libcrypto machinery.

Mount sessions are separate (up to 32 active volumes), retain a bounded copy
of the versioned host callback descriptor, and never recycle an opaque mount
cookie. The host must serialize unmount against operations on that mount.
Host I/O remains partition-relative, bounded, aligned and read-only. A real
EXT4 image regression exercises 512-byte and 4 KiB host logical sectors,
symlink resolution, file reading, directory listing and stale-handle refusal;
its SHA-256 must be unchanged. BOOT's independent OVMF/NVMe kernel-handoff
qualification remains in place.

**Production payload no longer links the V1 Defragmenter CLI / host service
adapters.** The historical bitmap/cache and worker regression code is built
only into test targets; it is not installed or included in `EXT4.so`.

This is not Linux root mounting, VFS registration, writable mounting,
formatting, journal recovery, or safe defragmentation. The operating-system
Kernel still needs a verified generic Common V2 ELF execution/VFS adapter to
mount this SO; do not fall back to the Linux native EXT4 implementation to
claim that step completed.

## Independent Common V2 permissions export (1.0.14)

The freestanding and hosted EXT4 format engine now returns real on-disk
file-mode and owner/group metadata through `METADATA.GET_MODE`.
This comes directly from the independent provider's validated inode reader,
not from any Kernel filesystem implementation. Native exec/permission and
ownership semantics can now be connected by the generic Common V2 VFS
adapter. Other metadata groups and writable mount behaviour still require
independent qualification and should not be represented as complete.

## Indexed-directory and release boundary corrections (1.0.15)

The portable Common V2 STAT operation returns actual directory byte sizes,
including populated indexed HTree directories, instead of zero. A disposable
real-image regression now uses e2fsck -D to construct a metadata-checksummed,
multi-block indexed /etc containing fstab. It checks CORE.STAT, CORE.LIST and
CORE.READ through two independent hosted mounts. This host qualification does
not prove that Zabbix's particular live EXT4 layout, firmware bridge, or
signed direct-boot handoff works; BOOT now retains the UEFI open error for
diagnosis rather than obscuring it behind a generic missing-file result.

The ordinary portable DEB and source-embedded native RUN now install the V2 SO
and its manual only. The ext4.ko source is retained for an explicitly selected
legacy migration experiment but is neither automatically compiled during
native installation nor published as a release asset. The CMake product version
is read from VERSION, and stale V1 ABI claims were removed from the release
config. The old V1 test harness remains source-only pending separate cleanup.

## Indexed HTree rejection qualification (1.0.16)

A separate disposable EXT4 image is created after successful read-only
qualification. Its HTree root checksum is deliberately corrupted by flipping
one bit, after independently resolving the physical index block with debugfs.
Common V2 must still read an unrelated file but return CORRUPT on fstab
lookup on both logical-sector configurations. The clean fixture now requires
a three-block indexed directory with two HTree leaves and a 665-byte fstab,
matching the observed Zabbix directory shape. CI guards both correctness
and image nonmutation. A passing virtual case is not a substitute for
testing the actual Zabbix disk and signed firmware path.

## 21 November 2026: Common V2 reader and qualification repairs (unreleased)

Changes are directly committed to `main` with no BOOT repository or physical
ESP/NVRAM change:

- `b75739d0`: `CORE.STAT`, `CORE.LIST` and `VOLUME.LABEL_GET` now
  return `IFS_FS_NO_SPACE` for valid entries whose output buffer is too small,
  rather than incorrectly returning `IFS_FS_INVALID`. A real-media
  Common V2 regression checks no partial entry writes and exact I/O error
  propagation from both probe/mount and reads after mounting.
- `0d01cdfe`: The actual no-import freestanding firmware ELF is loaded
  through its exported Common V2 API on the test host and exercised on the
  independently formatted real EXT4 image, HTree index, 4Kn transport and
  deliberately corrupted checksum fixture. The hosted .so carries an
  identical `.infiltrator_boot` ELF, compared byte-for-byte; no other
  filesystem driver substitutes for it. This is NOT proof of UEFI execution.
- `99fb2265`: The previously pinned BOOT 0.1.46 release download returned
  HTTP 404, and the release workflow's BOOT source commit was unreachable.
  Both obsolete dependencies were removed. The firmware and package
  workflows now download a real published BOOT DEB plus SHA256SUMS, verify
  exact package bytes, require its executable production `validate-module`
  and its x64 EFI image, and would exercise **the exact packaged EXT4.so**
  in disposable OVMF only after those gates pass.
- The corruption-image regression checks that neither the hosted nor the
  freestanding reader modifies the deliberately malformed source image.
  Release CI triggers only from main or manual dispatch, not pull requests.

**Current external blocker:** the latest verified BOOT release `v0.1.119`
stages a preview EFI binary but does **not** contain the Common V2 production
`validate-module` executable. EXT4's firmware workflow explicitly failed
at that gate on 21 November 2026, rather than misclassifying a V2 ELF compile
as successful BOOT validation. BOOT's current source README also states
that Common V2 module loading is not yet complete. Firmware validation,
real OVMF kernel/initramfs handoff, and native package release therefore
remain **unqualified**. The already-published `v1.0.16` remains unchanged.
No V1 or .ko runtime fallback has been introduced, and there has been
no weakening of a module signature, checksum or feature check.

## EXT4-only release qualification (1.0.17)

The native and portable EXT4 release now qualifies **only the EXT4 repository's own
Common V2 provider**, its real-media operations, and packaged `EXT4.so`. A
boot-manager implementation is a separate project and is never changed or
required for an EXT4 release. The previous external-BOOT-only OVMF workflow
was removed because it blocked a valid filesystem release on an unrelated
component. This does **not** claim OS boot, writable filesystems, journal
replay or other unimplemented EXT4 capabilities.

Native EXT4 inode reading now supports direct, single-, double- and
triple-indirect block maps as well as extent trees. Both profiles load actual
files from an EXT4 image upgraded from a classic block-map format, retaining
unchanged media bytes. Production runtime remains the independent Common V2
`EXT4.so` shared object; no legacy .ko or V1 adapter is installed.

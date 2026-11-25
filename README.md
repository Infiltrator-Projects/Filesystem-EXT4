# Filesystem-EXT4

## 1. Purpose and format scope

Fourth extended filesystem with extent-backed namespace and metadata integrity. This repository owns the format-specific implementation of `EXT4.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

Block groups, inode and extent trees, feature flags and metadata checksums determine interpretation. The reader validates clean EXT4 media with 1, 2 or 4 KiB blocks; it handles sparse ranges and symbolic links, rejecting layouts requiring replay or unsupported features.

## 3. Common V2 implementation and capabilities

Hosted and embedded freestanding V2 reader supports stat, list and read, with multiple hosted mount sessions. METADATA.GET_MODE reports mode and UID/GID. The native formatter requires a unique volume identity before writes. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

Writable root mounting, general metadata mutation, timestamp reporting and journal replay are separate qualifications; old V1 regression artifacts are not installed providers. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/EXT4.so` and `/System/Resources/Documentation/Filesystem-EXT4.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-EXT4/actions/runs/45520268651) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-EXT4/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/ext_v2_probe.c)
- [Native design](native/filesystems/ext4/DESIGN.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/EXT4.pages) and [PDF](docs/EXT4.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.

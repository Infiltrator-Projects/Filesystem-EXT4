# EXT4 engineering reference

## Evolution and format negotiation

Ext4 developed from ext2 and ext3; the ext4dev stage became stable in Linux 2.6.28 (2008). Extents, delayed allocation, larger address spaces, flexible groups and stronger integrity checks improve scalability and sequential placement but introduce unwritten-extent and feature-admission complexity. Ext4 is not a pooled or general copy-on-write snapshot filesystem.

Existing volumes can retain indirect-mapped inodes: a filesystem feature permits extent mapping, while the per-inode flag selects it. Interpret compatible, incompatible and read-only-compatible feature words independently. Unknown incompatible flags forbid mounting; unknown read-only-compatible flags prohibit writes. Distinguish journal mode and physical format from provider capability; recognition alone does not prove implementation or safety.

## Superblock and geometry

The primary superblock begins at volume byte 1024 and occupies 1024 bytes. All filesystem integers are little-endian. Relative offsets: inode count 0x00, low block count 0x04, reserved blocks 0x08, free blocks 0x0C, free inodes 0x10, first data block 0x14, log block size 0x18, log cluster size 0x1C, blocks per group 0x20, clusters per group 0x24, inodes per group 0x28. The magic 0xEF53 is at 0x38; revision at 0x4C, first ordinary inode at 0x54, inode size at 0x58, feature words at 0x5C/0x60/0x64, UUID at 0x68. Block bytes equal 1024 times 2 to the power log block size. Check counts and multiply using checked arithmetic.

## Extended geometry and backup selection

The group descriptor size is at superblock offset 0xFE, high total, reserved and free block counts at 0x150/0x154/0x158, checksum at 0x3FC. When the 64-bit feature is active, join high and low halves rather than interpreting either independently. Group count is the ceiling of (total blocks minus first data block) divided by blocks per group. Sparse-super backup placement depends on feature flags: traditional sparse-super preserves copies in groups 0, 1 and powers of 3, 5 and 7; sparse-super2 specifies selected backup groups. META_BG alters descriptor placement. FLEX_BG permits metadata outside its owning group's nominal data region. Never blindly reconstruct descriptor addresses using the classic contiguous layout.

## Group descriptors and allocation

The classic descriptor's low bitmap addresses are at 0, 4 and 8 (block, inode and inode table); free block and inode counts at 12 and 14, used-directory count at 16, flags at 18. Bitmap checksums begin at 24 and 26, unused inode count at 28, descriptor checksum at 30. The 64-byte extension begins with high address words at 32, 36 and 40; count high halves at 44, 46 and 48. A bitmap set bit denotes allocation; BIGALLOC makes the block bitmap represent clusters rather than individual blocks. Lazy-init flags govern whether bitmap content is valid. Reserve metadata explicitly, account for the partial final group and reconcile per-group counters with global totals.

## Inodes and exact location

Inode numbers begin at one. Group equals floor((inode-1)/inodes_per_group), slot equals (inode-1) modulo inodes_per_group. Read the table address from its descriptor, then add slot times inode size. Inode 2 is root. Relative inode offsets: mode 0, UID low 2, size low 4, access/change/modification/deletion times 8/12/16/20, GID low 24, links 26, block count low 28, flags 32, mapping union 40 (60 bytes), generation 100, ACL block low 104, size high 108. Extra fields beyond 128 bytes depend on the declared extra inode size. With 8192 inodes per group, inode 8200 belongs to group 1, slot 7; with 256-byte records its offset is 1792 bytes into that inode table.

## Extent records and tree traversal

The mapping union starts with a 12-byte extent header when the inode extent flag is set. Fields: magic 0xF30A at 0, entry count at 2, capacity at 4, depth at 6, generation at 8. Each 12-byte leaf record stores logical block at 0, encoded length at 4, physical high 16 bits at 6, physical low 32 bits at 8. Encoded lengths 1 through 32768 are initialised; greater values represent unwritten extents with 32768 subtracted. Index records carry logical key at 0, child low 32 bits at 4, high 16 at 8 and reserved at 10. Choose the greatest key not exceeding the requested logical block. Validate sorted intervals, depth, capacity and checksum before following a child. The inode root can contain four records.

## Mapping, holes and file length

An extent logical 100, physical 50000, length 20 maps logical block 108 to physical block 50008. With 4096-byte blocks, byte offset 100 inside that block maps to volume byte 204832868. Unwritten extents and absent logical intervals read as zero, regardless of stale media bytes. The old mapping instead uses twelve direct pointers followed by single, double and triple indirect pointers; each indirect block contains block_size/4 addresses. Inline data and short symlinks can use inode storage, so the mapping union cannot be interpreted solely from its bytes. Clip every read to logical file length, not allocated capacity.

## Directory records and indexed lookup

Ordinary directory records contain 32-bit inode, 16-bit record length, 8-bit name length and 8-bit file type, followed by the name, when FILETYPE is enabled. Older directories use a 16-bit name length. A zero inode is unused space. Records must remain in a filesystem block, respect alignment and contain their claimed names. HTree directories use hash-to-logical-directory-block indexes; the root embeds dot and dot-dot entries plus hash/version and level information. Select by hash, then compare actual names in the candidate leaf and handle collisions. Index blocks have distinct checksum tails. Encrypted or casefolded names require the feature-specific comparison and hash semantics.

## Metadata integrity

EXT4 metadata checksums generally use CRC32C, with UUID-derived or explicitly recorded seed where applicable. Superblock checksum covers bytes before its checksum field. Inode, extent-block and directory checksums also bind metadata to inode identity and generation; group descriptor checksums bind group identity and descriptor contents. Bitmap checksums reside in group descriptors and may be truncated. Legacy GDT_CSUM uses CRC16 rather than the modern descriptor checksum scheme. Treat checksum fields and coverage according to the exact structure, including any required zeroing or exclusion. A correct checksum is necessary but not sufficient: validate geometry, ownership and semantic invariants independently.

## JBD2 journal and recovery

JBD2 structures are big-endian, unlike EXT4 metadata. A journal block begins with magic 0xC03B3998, block type and transaction sequence, three 32-bit words. The circular journal contains descriptor blocks naming home-block targets, logged data, revokes and commits. Tags vary with checksum and 64-bit features. Recovery identifies complete committed transactions, builds revoke state and applies eligible images in sequence, then checkpoints durable home blocks before clearing journal state. Reject corrupted tags, invalid target blocks, incompatible journal features and incomplete transactions. An external journal must match its declared identity. Ordered, writeback and full-data journalling differ in when user data becomes durable.

## Mutation, allocation and relocation

Delayed allocation and multiblock allocation select runs when dirty data is written, improving locality. Preserve the distinction between an unwritten preallocated extent and a hole as described in Mapping, holes and file length. Allocation updates bitmap, group counts, extent nodes and inode metadata with required journal ordering. Truncate and unlink may need orphan tracking until cleanup completes. Relocation must reserve a destination, copy and verify bytes, publish replacement mappings transactionally and release old blocks only after publication is durable. Preserve metadata checksums and all applicable allocation-unit restrictions. Extent merging is valid only when logical/physical continuity and written state agree. Atomic I/O guarantees depend on host capabilities, not just extent layout.

## Additional feature families

Extended attributes may occupy inode extra space, external blocks or EA inodes. Inline data can require conversion to extent storage. MMP prevents competing writers when its protocol is supported. Fast commits encode selected metadata changes as TLV records and require idempotent replay; unsupported operations require a full commit. Orphan files provide arrays of pending inode numbers with integrity metadata. Project quotas, fscrypt, verity, casefold and large-directory features each impose additional rules. No feature is safe to ignore for a writable mount merely because basic directory reads succeed. Each requires its own field-level and behavioural verification.

## Implementation and qualification boundary

At historical revision dad78c3dd93a the hosted and embedded reader admitted clean 1, 2 and 4 KiB block media, sparse ranges and symbolic links; hosted multiple mounts and GET_MODE reported UID/GID/mode. Formatter identity was obtained before mutation. Journal replay, timestamp reporting and writable root support were separate questions.

Common V2 is the independent provider interface: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate real operation masks and media profiles. Use bounded host I/O, exclusive unmounted ownership and durable flush for mutation; distinguish unsupported, corrupt, I/O and stale-handle results. Test Media and Defragmenter use the same engine. Hosted, freestanding, BOOT, kernel, recovery and package qualification are independent. Current capability and release status belongs to the repository README and exact-source qualification records; historical evidence is not a current status claim.

Historical source or test reference: https://github.com/Infiltrator-Projects/Filesystem-EXT4/actions/runs/4552028651 .

## Independent qualification and sources

Use e2fsprogs mke2fs, debugfs and e2fsck as independent fixture and checking tools, not as hidden production providers. Test 1/2/4 KiB block sizes, multi-group and flex layouts, extent depth transitions, unwritten and sparse data, legacy mapping, directory hash collisions, damaged checksums, journal revokes, interrupted writes and feature rejection. Compare raw file hashes and reconstructed ownership before and after mutation. Primary technical reference: https://docs.kernel.org/filesystems/ext4/ ; detailed journal and layout chapters are linked there. Independent implementation reference: https://github.com/tytso/e2fsprogs . The existing native/filesystems/ext4/DESIGN.md is retained as the project's design and feature-boundary record. Older EXT2 and EXT3 documents are preserved in the historical directory, not represented as EXT4 manuals.

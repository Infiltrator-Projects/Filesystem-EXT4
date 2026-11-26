/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded EXT4 boot reader using only Common V2 host services. */
#include <stdbool.h>
#if defined(IFS_EXT4_HOSTED_BUILD)
#include <string.h>
#endif
#include <infiltratr/filesystem_module_v2.h>
#include <stdatomic.h>

/* Protect shared native session state against concurrent and reentrant
 * operations. Never wait while a host callback is executing: report BUSY
 * instead of risking deadlock or teardown of a live volume. This does not
 * qualify parallel I/O across volumes. */
static atomic_flag v2_operation_gate=ATOMIC_FLAG_INIT;
static int v2_enter_operation(void)
{
    return !atomic_flag_test_and_set_explicit(&v2_operation_gate,memory_order_acquire);
}
static int v2_leave_operation(int rc)
{
    atomic_flag_clear_explicit(&v2_operation_gate,memory_order_release);
    return rc;
}

#include "../../native/filesystems/ext4/core/ext4_core.h"

enum {
    MAX_BLOCK = 4096, MAX_PATH = 4096, MAX_DIRECTORY = 16 * 1024 * 1024,
    ROOT = 2, EXTENT_MAGIC = 0xf30a, EXTENT_RECORD = 12, MAX_DEPTH = 5,
    FILE_TYPE = 0170000, FILE_REGULAR = 0100000, FILE_DIRECTORY = 0040000,
    FILE_LINK = 0120000, INDEX_FLAG = 0x1000, EXTENTS_FLAG = 0x80000,
    UNSUPPORTED_INODE_FLAGS = 0x800 | 0x100000 | 0x10000000 | 0x40000000
};

typedef struct {
    const IfsFsHostRuntimeV2 *host;
    IfsFsVolumeInfoV2 info;
    uint64_t blocks, groups;
    uint32_t first, inodes, inodes_per_group, blocks_per_group, seed;
    uint32_t compat, incompat, ro_compat, backup_groups[2];
    uint16_t reserved_gdt;
    uint16_t inode_bytes, desc_bytes;
    bool wide, checksum, filetype;
    char label[17];
} Volume;

typedef struct {
    uint64_t bytes;
    uint32_t flags, seed;
    uint16_t type, mode;
    uint32_t owner, group;
    uint8_t map[60];
} Inode;

typedef struct {
    uint32_t number, length;
    char name[256];
} Dirent;

/* One portable EXT4 format engine backs hosted and firmware profiles.
 * Slots are bounded and do not reuse opaque mount cookies. The host must
 * serialize operations on a mount with unmount, as required by the lifecycle
 * contract. No OS-native filesystem code or userspace proxy is involved. */
enum { MAX_MOUNTS = 32 };
typedef struct {
    Volume volume;
    IfsFsHostRuntimeV2 host;
    uintptr_t cookie;
} MountSlot;
static MountSlot mounts[MAX_MOUNTS];
static uintptr_t generation;

#if defined(IFS_EXT4_HOSTED_BUILD)
__attribute__((used, retain, section(".ifsmeta")))
static const char ext4_identity[] = "IFS-MODULE:ext4:common2";
#endif

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)u16(p) | ((uint32_t)u16(p + 2) << 16);
}

static uint32_t crc32c(uint32_t crc, const uint8_t *p, uint32_t bytes)
{
    while (bytes--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & UINT32_C(0x82f63b78));
    }
    return crc;
}

static uint32_t crc_number(uint32_t crc, uint32_t number)
{
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = (uint8_t)(number >> (8 * i));
    return crc32c(crc, bytes, sizeof(bytes));
}

static int open_volume(Volume *v, const IfsFsHostRuntimeV2 *host)
{
    uint8_t sb[1024];
    const uint32_t ext4 = IFS_EXT4_FEATURE_INCOMPAT_EXTENTS |
        IFS_EXT4_FEATURE_INCOMPAT_64BIT | IFS_EXT4_FEATURE_INCOMPAT_FLEX_BG |
        IFS_EXT4_FEATURE_INCOMPAT_CSUM_SEED;
    const uint32_t supported = ext4 | IFS_EXT4_FEATURE_INCOMPAT_FILETYPE |
        IFS_EXT4_FEATURE_INCOMPAT_EA_INODE | IFS_EXT4_FEATURE_INCOMPAT_LARGEDIR;
    if (!ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    if (host->volume_bytes < 4096)
        return IFS_FS_NOT_FOUND;
    int rc = ifs_fs_host_read_bytes_v2(host, 1024, sb, sizeof(sb));
    if (rc != IFS_FS_OK)
        return rc;
    if (u16(sb + 56) != 0xef53 || !(u32(sb + 96) & ext4))
        return IFS_FS_NOT_FOUND;
    uint32_t flags = u32(sb + 96), ro = u32(sb + 100);
    if ((flags & ~supported) || ifs_ext4_unsupported_ro_compat_features(ro) ||
        (ro & (IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC | IFS_EXT4_FEATURE_RO_COMPAT_GDT_CSUM |
               IFS_EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT)))
        return IFS_FS_UNSUPPORTED;
    bool checksum = (ro & IFS_EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
    if (checksum && (u32(sb + 72) || sb[0x175] != 1 ||
                     u32(sb + 1020) != crc32c(UINT32_MAX, sb, 1020)))
        return IFS_FS_CORRUPT;
    uint32_t shift = u32(sb + 24);
    if (shift > 2 || u32(sb + 28) != shift)
        return IFS_FS_UNSUPPORTED;
    uint32_t bytes = 1024u << shift;
    uint32_t first = u32(sb + 20), inodes = u32(sb), ipg = u32(sb + 40), bpg = u32(sb + 32);
    bool wide = (flags & IFS_EXT4_FEATURE_INCOMPAT_64BIT) != 0;
    uint16_t inode_bytes = u16(sb + 88), desc_bytes = wide ? u16(sb + 254) : 32;
    uint64_t blocks = u32(sb + 4), free_blocks = u32(sb + 12), groups;
    if (wide) {
        blocks |= (uint64_t)u32(sb + 0x150) << 32;
        free_blocks |= (uint64_t)u32(sb + 0x158) << 32;
    }
    if (first != (shift == 0 ? 1u : 0u) || blocks <= first || inodes < ROOT ||
        free_blocks > blocks || blocks > host->volume_bytes / bytes ||
        ifs_ext4_validate_inode_geometry(bytes, inode_bytes, u32(sb + 84)) ||
        ifs_ext4_validate_group_geometry(bytes, inode_bytes, desc_bytes, wide, bpg, ipg) ||
        ifs_ext4_validate_cluster_geometry(bytes, bytes, 0, bpg, u32(sb + 36)) ||
        ifs_ext4_validate_layout(bytes, u16(sb + 206), blocks, first, shift, 1,
                                 bpg, bytes / desc_bytes, ipg, inodes, &groups) ||
        groups * desc_bytes > (blocks - first - 1) * bytes)
        return IFS_FS_CORRUPT;
    v->host = host;
    v->blocks = blocks;
    v->groups = groups;
    v->compat = u32(sb + 92);
    v->incompat = flags;
    v->ro_compat = ro;
    v->reserved_gdt = u16(sb + 206);
    v->backup_groups[0] = u32(sb + 0x24c);
    v->backup_groups[1] = u32(sb + 0x250);
    v->first = first;
    v->inodes = inodes;
    v->inodes_per_group = ipg;
    v->blocks_per_group = bpg;
    v->inode_bytes = inode_bytes;
    v->desc_bytes = desc_bytes;
    v->wide = wide;
    v->checksum = checksum;
    v->filetype = (flags & IFS_EXT4_FEATURE_INCOMPAT_FILETYPE) != 0;
    v->seed = flags & IFS_EXT4_FEATURE_INCOMPAT_CSUM_SEED ?
        u32(sb + 0x270) : crc32c(UINT32_MAX, sb + 104, 16);
    v->info = (IfsFsVolumeInfoV2){
        .structure_bytes = sizeof(v->info), .variant = 4, .max_name_bytes = 255,
        .block_bytes = bytes, .total_bytes = blocks * bytes, .free_bytes = free_blocks * bytes
    };
    for (unsigned i = 0; i < 16; ++i)
        v->label[i] = (char)sb[120 + i];
    v->label[16] = 0;
    return IFS_FS_OK;
}

static int read_inode(const Volume *v, uint32_t number, Inode *inode)
{
    uint8_t raw[MAX_BLOCK];
    if (!number || number > v->inodes)
        return IFS_FS_CORRUPT;
    uint32_t bytes = v->info.block_bytes;
    uint32_t group = (number - 1) / v->inodes_per_group;
    uint32_t index = (number - 1) % v->inodes_per_group;
    uint64_t desc = ((uint64_t)v->first + 1) * bytes + (uint64_t)group * v->desc_bytes;
    int rc = ifs_fs_host_read_bytes_v2(v->host, desc, raw, v->desc_bytes);
    if (rc != IFS_FS_OK)
        return rc;
    if (v->checksum) {
        uint16_t expected = u16(raw + 30);
        raw[30] = raw[31] = 0;
        if ((uint16_t)crc32c(crc_number(v->seed, group), raw, v->desc_bytes) != expected)
            return IFS_FS_CORRUPT;
    }
    uint64_t table = u32(raw + 8);
    if (v->wide)
        table |= (uint64_t)u32(raw + 40) << 32;
    uint64_t offset = (uint64_t)index * v->inode_bytes;
    if (!table || table >= v->blocks || offset / bytes >= v->blocks - table)
        return IFS_FS_CORRUPT;
    rc = ifs_fs_host_read_bytes_v2(v->host, table * bytes + offset, raw, v->inode_bytes);
    if (rc != IFS_FS_OK)
        return rc;
    if (!u16(raw + 26) || u32(raw + 20))
        return IFS_FS_CORRUPT;
    inode->seed = crc_number(crc_number(v->seed, number), u32(raw + 100));
    if (v->checksum) {
        uint16_t extra = v->inode_bytes > 128 ? u16(raw + 128) : 0;
        if ((extra & 3) || extra > v->inode_bytes - 128)
            return IFS_FS_CORRUPT;
        uint32_t expected = u16(raw + 124);
        raw[124] = raw[125] = 0;
        if (extra >= 4) {
            expected |= (uint32_t)u16(raw + 130) << 16;
            raw[130] = raw[131] = 0;
        }
        uint32_t crc = crc32c(inode->seed, raw, v->inode_bytes);
        if ((extra >= 4 ? crc : (uint16_t)crc) != expected)
            return IFS_FS_CORRUPT;
    }
    inode->mode = u16(raw);
    inode->type = inode->mode & FILE_TYPE;
    /* EXT4 inode ownership is split across the original 16-bit fields
     * and osd2 high bits. The freestanding provider supplies permissions,
     * not the VFS adapter or a copied format-specific Kernel parser. */
    inode->owner = (uint32_t)u16(raw + 2) |
                   ((uint32_t)u16(raw + 120) << 16);
    inode->group = (uint32_t)u16(raw + 24) |
                   ((uint32_t)u16(raw + 122) << 16);
    inode->flags = u32(raw + 32);
    inode->bytes = u32(raw + 4) | ((uint64_t)u32(raw + 108) << 32);
    if (inode->flags & UNSUPPORTED_INODE_FLAGS)
        return IFS_FS_UNSUPPORTED;
    for (unsigned i = 0; i < sizeof(inode->map); ++i)
        inode->map[i] = raw[40 + i];
    return IFS_FS_OK;
}

/* EXT4 may retain older block-mapped inodes even when the volume supports
 * extents (for example files preserved during an EXT3-to-EXT4 upgrade).
 * Read the 12 direct pointers and single/double/triple indirect trees from
 * their native i_block encoding. BOOT cannot assume every file is extents-
 * mapped just because s_feature_incompat advertises EXTENTS.
 *
 * Each child is a 32-bit physical block number. Never follow out-of-volume
 * pointers. A zero pointer is a sparse hole, which read_data zero-fills. */
static int indirect_block(const Volume *v, const Inode *inode,
                          uint32_t logical, uint64_t *physical)
{
    const uint32_t block_bytes = v->info.block_bytes;
    const uint32_t per_block = block_bytes / 4u;
    uint32_t path[4] = {0};
    uint32_t ignored_boundary;
    uint32_t bits = 0;
    while ((1u << bits) < per_block)
        ++bits;
    const int depth = ifs_ext4_indirect_block_path(
        logical, per_block, bits, path, &ignored_boundary);
    if (depth < 1 || depth > 4)
        return IFS_FS_UNSUPPORTED;
    if (path[0] >= sizeof(inode->map) / 4u)
        return IFS_FS_CORRUPT;

    uint32_t block = u32(inode->map + 4u * path[0]);
    for (int level = 1; level < depth; ++level) {
        if (!block) {
            *physical = 0;
            return IFS_FS_OK;
        }
        if ((uint64_t)block >= v->blocks || path[level] >= per_block)
            return IFS_FS_CORRUPT;
        uint8_t pointer[4];
        uint64_t at = (uint64_t)block * block_bytes +
                      (uint64_t)path[level] * sizeof(pointer);
        const int rc = ifs_fs_host_read_bytes_v2(v->host, at, pointer,
                                                 sizeof(pointer));
        if (rc != IFS_FS_OK)
            return rc;
        block = u32(pointer);
    }
    if (block && (uint64_t)block >= v->blocks)
        return IFS_FS_CORRUPT;
    *physical = block;
    return IFS_FS_OK;
}

/* The inode stores the root extent node when EXTENTS_FL is enabled.
 * Children occupy whole blocks. A sparse or unwritten logical block maps to
 * physical block zero, using the same Common V2 byte-range reader. */
static int extent(const Volume *v, const Inode *inode, uint32_t logical, uint64_t *physical)
{
    if (!(inode->flags & EXTENTS_FLAG))
        return indirect_block(v, inode, logical, physical);
    uint8_t block[MAX_BLOCK];
    const uint8_t *node = inode->map;
    uint32_t bytes = sizeof(inode->map);
    uint16_t depth = u16(node + 6);
    if (depth > MAX_DEPTH)
        return IFS_FS_CORRUPT;
    for (;;) {
        uint16_t count = u16(node + 2), capacity = u16(node + 4);
        if (u16(node) != EXTENT_MAGIC || u16(node + 6) != depth ||
            !capacity || count > capacity || capacity > (bytes - 12) / EXTENT_RECORD ||
            (!count && (depth || node == block)))
            return IFS_FS_CORRUPT;
        if (node == block && v->checksum) {
            uint32_t tail = 12 + capacity * EXTENT_RECORD;
            if (tail > bytes - 4 || u32(node + tail) != crc32c(inode->seed, node, tail))
                return IFS_FS_CORRUPT;
        }
        uint64_t previous_end = 0, selected = 0;
        for (unsigned i = 0; i < count; ++i) {
            const uint8_t *row = node + 12 + i * EXTENT_RECORD;
            uint32_t first = u32(row);
            if (first < previous_end)
                return IFS_FS_CORRUPT;
            if (depth) {
                uint64_t child = u32(row + 4) | ((uint64_t)u16(row + 8) << 32);
                if (!child || child >= v->blocks)
                    return IFS_FS_CORRUPT;
                previous_end = (uint64_t)first + 1;
                if (logical >= first)
                    selected = child;
            } else {
                uint16_t length = u16(row + 4);
                bool unwritten = length > 32768;
                if (unwritten)
                    length -= 32768;
                uint64_t start = u32(row + 8) | ((uint64_t)u16(row + 6) << 32);
                previous_end = (uint64_t)first + length;
                if (!length || previous_end > (UINT64_C(1) << 32) ||
                    !start || start >= v->blocks || length > v->blocks - start)
                    return IFS_FS_CORRUPT;
                if (!unwritten && logical >= first && logical < previous_end)
                    selected = start + (logical - first);
            }
        }
        if (!depth || !selected) {
            *physical = selected;
            return IFS_FS_OK;
        }
        bytes = v->info.block_bytes;
        int rc = ifs_fs_host_read_bytes_v2(v->host, selected * bytes, block, bytes);
        if (rc != IFS_FS_OK)
            return rc;
        node = block;
        --depth;
    }
}

static int read_data(const Volume *v, const Inode *inode, uint64_t offset, void *out, uint32_t bytes)
{
    if ((!out && bytes) || offset > inode->bytes || bytes > inode->bytes - offset)
        return IFS_FS_INVALID;
    uint8_t *dest = out;
    uint32_t block_bytes = v->info.block_bytes;
    while (bytes) {
        uint64_t logical = offset / block_bytes, physical;
        uint32_t start = (uint32_t)(offset % block_bytes), chunk = block_bytes - start;
        if (chunk > bytes)
            chunk = bytes;
        if (logical > UINT32_MAX)
            return IFS_FS_UNSUPPORTED;
        int rc = extent(v, inode, (uint32_t)logical, &physical);
        if (rc != IFS_FS_OK)
            return rc;
        if (physical) {
            rc = ifs_fs_host_read_bytes_v2(v->host, physical * block_bytes + start, dest, chunk);
            if (rc != IFS_FS_OK)
                return rc;
        } else {
            for (uint32_t i = 0; i < chunk; ++i)
                dest[i] = 0;
        }
        offset += chunk;
        dest += chunk;
        bytes -= chunk;
    }
    return IFS_FS_OK;
}

static int directory_end(const Volume *v, const Inode *dir, const uint8_t *p,
                         uint32_t block, uint32_t *end)
{
    uint32_t bytes = v->info.block_bytes;
    *end = bytes;
    if (v->checksum && !u32(p + bytes - 12) &&
        u16(p + bytes - 8) == 12 && u16(p + bytes - 6) == 0xde00) {
        *end -= 12;
        return u32(p + bytes - 4) == crc32c(dir->seed, p, *end) ?
            IFS_FS_OK : IFS_FS_CORRUPT;
    }
    if (!(dir->flags & INDEX_FLAG))
        return v->checksum ? IFS_FS_CORRUPT : IFS_FS_OK;

    /* Directory index blocks use fake dirents. Verify the index checksum
     * before skipping them. Leaf blocks have the dirent checksum above. */
    uint32_t header;
    if (!block) {
        if (u16(p + 4) != 12 || u16(p + 16) != bytes - 12 || u32(p + 24) || p[29] != 8)
            return IFS_FS_CORRUPT;
        header = 32;
    } else if (!u32(p) && u16(p + 4) == bytes) {
        header = 8;
    } else {
        return v->checksum ? IFS_FS_CORRUPT : IFS_FS_OK;
    }
    uint32_t limit = u16(p + header), count = u16(p + header + 2);
    uint32_t trailer = v->checksum ? 8 : 0;
    if (!count || count > limit || limit != (bytes - header - trailer) / 8)
        return IFS_FS_CORRUPT;
    if (v->checksum) {
        uint32_t tail = header + limit * 8;
        uint32_t crc = crc32c(dir->seed, p, header + count * 8);
        crc = crc32c(crc, p + tail, 4);
        if (u32(p + tail + 4) != crc_number(crc, 0))
            return IFS_FS_CORRUPT;
    }
    return IFS_FS_OK;
}

static bool equal(const uint8_t *disk, const char *name, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i)
        if (disk[i] != (uint8_t)name[i])
            return false;
    return true;
}

static int find_entry(const Volume *v, const Inode *dir, const char *name,
                      uint32_t length, uint32_t index, Dirent *entry)
{
    if (dir->type != FILE_DIRECTORY)
        return IFS_FS_NOT_FOUND;
    uint32_t bytes = v->info.block_bytes;
    if (!dir->bytes || dir->bytes % bytes)
        return IFS_FS_CORRUPT;
    if (dir->bytes > MAX_DIRECTORY)
        return IFS_FS_UNSUPPORTED;
    uint8_t buffer[MAX_BLOCK];
    for (uint32_t block = 0; block < dir->bytes / bytes; ++block) {
        int rc = read_data(v, dir, (uint64_t)block * bytes, buffer, bytes);
        if (rc != IFS_FS_OK)
            return rc;
        uint32_t end;
        rc = directory_end(v, dir, buffer, block, &end);
        if (rc != IFS_FS_OK)
            return rc;
        for (uint32_t pos = 0; pos < end;) {
            if (end - pos < 8)
                return IFS_FS_CORRUPT;
            const uint8_t *row = buffer + pos;
            uint32_t number = u32(row), record = u16(row + 4), len = row[6];
            if ((record & 3) || record < 8 || record > end - pos ||
                number > v->inodes || (number && (!len ||
                record < ifs_ext4_directory_record_min_length(len, 0) ||
                (!v->filetype && row[7]))))
                return IFS_FS_CORRUPT;
            pos += record;
            if (!number)
                continue;
            for (uint32_t i = 0; i < len; ++i)
                if (!row[8 + i] || row[8 + i] == '/')
                    return IFS_FS_CORRUPT;
            bool dot = row[8] == '.' && (len == 1 || (len == 2 && row[9] == '.'));
            if (name ? len == length && equal(row + 8, name, len) : !dot && index-- == 0) {
                entry->number = number;
                entry->length = len;
                for (uint32_t i = 0; i < len; ++i)
                    entry->name[i] = (char)row[8 + i];
                entry->name[len] = 0;
                return IFS_FS_OK;
            }
        }
    }
    return name ? IFS_FS_NOT_FOUND : IFS_FS_END;
}

/* Keep the containing directory for relative links, and resolve '..' on
 * disk after following a link. Cap both expanded length and link hops. */
static int resolve(const Volume *v, const char *path, Inode *current, Dirent *leaf)
{
    char pending[MAX_PATH];
    if (!path || *path != '/')
        return IFS_FS_INVALID;
    uint32_t length = 0;
    while (length < MAX_PATH && path[length]) {
        pending[length] = path[length];
        ++length;
    }
    if (length == MAX_PATH)
        return IFS_FS_INVALID;
    pending[length] = 0;
    uint32_t pos = 0, links = 0;
    int rc = read_inode(v, ROOT, current);
    if (rc != IFS_FS_OK)
        return rc;
    if (current->type != FILE_DIRECTORY)
        return IFS_FS_CORRUPT;
    leaf->number = ROOT;
    leaf->length = 0;
    leaf->name[0] = 0;
    while (pos < length) {
        if (current->type != FILE_DIRECTORY)
            return IFS_FS_NOT_FOUND;
        while (pending[pos] == '/')
            ++pos;
        if (pos == length)
            break;
        uint32_t start = pos;
        while (pos < length && pending[pos] != '/')
            ++pos;
        if (pos - start > 255)
            return IFS_FS_INVALID;
        rc = find_entry(v, current, pending + start, pos - start, 0, leaf);
        if (rc != IFS_FS_OK)
            return rc;
        Inode next;
        rc = read_inode(v, leaf->number, &next);
        if (rc != IFS_FS_OK)
            return rc;
        if (next.type != FILE_LINK) {
            *current = next;
            continue;
        }
        uint32_t rest = length - pos;
        if (++links > 16 || !next.bytes || next.bytes >= MAX_PATH - rest)
            return IFS_FS_UNSUPPORTED;
        uint32_t target = (uint32_t)next.bytes;
        if (target < pos) {
            for (uint32_t i = 0; i <= rest; ++i)
                pending[target + i] = pending[pos + i];
        } else {
            for (uint32_t i = rest + 1; i > 0; --i)
                pending[target + i - 1] = pending[pos + i - 1];
        }
        if (target <= sizeof(next.map) && !(next.flags & EXTENTS_FLAG)) {
            for (uint32_t i = 0; i < target; ++i)
                pending[i] = (char)next.map[i];
        } else {
            rc = read_data(v, &next, 0, pending, target);
            if (rc != IFS_FS_OK)
                return rc;
        }
        for (uint32_t i = 0; i < target; ++i)
            if (!pending[i])
                return IFS_FS_CORRUPT;
        if (pending[0] == '/') {
            rc = read_inode(v, ROOT, current);
            if (rc != IFS_FS_OK)
                return rc;
            leaf->length = 0;
            leaf->name[0] = 0;
        }
        length = target + rest;
        pos = 0;
    }
    return IFS_FS_OK;
}

static int entry_info(const Inode *inode, const Dirent *name, IfsFsEntryV2 *entry)
{
    if (!entry || entry->structure_bytes < sizeof(*entry) || !entry->name)
        return IFS_FS_INVALID;
    /* This is a valid existing entry: an undersized caller buffer is a
     * capacity error, not a malformed API argument or missing on-disk data.
     * Never partially populate a Common V2 entry on NO_SPACE. */
    /* The root inode has no directory-entry component. Common V2 STAT
     * exposes it as "/", never as a zero-length filename. */
    const uint32_t length = name->length ? name->length : 1u;
    if (entry->name_capacity <= length)
        return IFS_FS_NO_SPACE;
    switch (inode->type) {
    case FILE_REGULAR: entry->type = IFS_FS_ENTRY_FILE; break;
    case FILE_DIRECTORY: entry->type = IFS_FS_ENTRY_DIRECTORY; break;
    case FILE_LINK: entry->type = IFS_FS_ENTRY_LINK; break;
    default: return IFS_FS_UNSUPPORTED;
    }
    /* EFI_FILE_INFO and Common V2 STAT must report the real directory
     * byte size. Returning zero hid multi-block/Htree layout from callers. */
    entry->size_bytes = inode->bytes;
    entry->attributes = 0;
    entry->name_bytes = length;
    if (!name->length) {
        entry->name[0] = '/';
        entry->name[1] = 0;
    } else {
        for (uint32_t i = 0; i <= name->length; ++i)
            entry->name[i] = name->name[i];
    }
    return IFS_FS_OK;
}

/* Only compare opaque cookie values; never dereference user-supplied
 * handles. A cookie is never recycled even when its slot is reused. */
static Volume *mounted(void *handle)
{
    uintptr_t value = (uintptr_t)handle;
    if (!value) return NULL;
    for (unsigned i = 0; i < MAX_MOUNTS; ++i)
        if (mounts[i].cookie == value)
            return &mounts[i].volume;
    return NULL;
}

static int probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    Volume candidate;
    if (!variant) return IFS_FS_INVALID;
    int rc = open_volume(&candidate, host);
    *variant = rc == IFS_FS_OK ? 4u : 0u;
    return rc;
}
static int guarded_probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    if (variant) *variant=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(probe(host,variant));
}


static int mount(const IfsFsHostRuntimeV2 *host, void **handle,
                 IfsFsVolumeInfoV2 *info)
{
    if (!handle || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *handle = NULL;
    if (!ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    if (!(host->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
    if (generation == UINTPTR_MAX)
        return IFS_FS_BUSY;
    MountSlot *slot = NULL;
    for (unsigned i = 0; i < MAX_MOUNTS; ++i)
        if (!mounts[i].cookie) {
            slot = &mounts[i];
            break;
        }
    if (!slot)
        return IFS_FS_BUSY;

    /* A host may pass a short, size-versioned Common V2 descriptor on the
     * stack. Retain a bounded copy for the entire mount lifetime. */
    const uint32_t n = host->structure_bytes < sizeof(slot->host) ?
        host->structure_bytes : sizeof(slot->host);
    uint8_t *destination = (uint8_t *)&slot->host;
    const uint8_t *source = (const uint8_t *)host;
    for (uint32_t i = 0; i < sizeof(slot->host); ++i)
        destination[i] = i < n ? source[i] : 0;
    slot->host.structure_bytes = n;
    int rc = open_volume(&slot->volume, &slot->host);
    if (rc != IFS_FS_OK) {
        slot->volume.host = NULL;
        return rc;
    }
    Inode root;
    rc = read_inode(&slot->volume, ROOT, &root);
    if (rc != IFS_FS_OK || root.type != FILE_DIRECTORY) {
        slot->volume.host = NULL;
        return rc != IFS_FS_OK ? rc : IFS_FS_CORRUPT;
    }
    uintptr_t cookie;
    do {
        if (generation == UINTPTR_MAX) {
            slot->volume.host = NULL;
            return IFS_FS_BUSY;
        }
        cookie = (++generation) ^ (uintptr_t)(void *)mounts;
    } while (!cookie);
    slot->cookie = cookie;
    *handle = (void *)cookie;
    *info = slot->volume.info;
    return IFS_FS_OK;
}
static int guarded_mount(const IfsFsHostRuntimeV2 *host, void **handle,
                 IfsFsVolumeInfoV2 *info)
{
    if (handle) *handle=NULL;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(mount(host,handle,info));
}


static int unmount(void *handle)
{
    uintptr_t token = (uintptr_t)handle;
    if (!token) return IFS_FS_INVALID;
    for (unsigned i = 0; i < MAX_MOUNTS; ++i)
        if (mounts[i].cookie == token) {
            mounts[i].cookie = 0;
            mounts[i].volume.host = NULL;
            return IFS_FS_OK;
        }
    return IFS_FS_INVALID;
}
static int guarded_unmount(void *handle)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(unmount(handle));
}


static int stat_path(void *handle, const char *path, IfsFsEntryV2 *entry)
{
    Volume *v = mounted(handle);
    if (!v) return IFS_FS_INVALID;
    Inode inode;
    Dirent name;
    int rc = resolve(v, path, &inode, &name);
    return rc == IFS_FS_OK ? entry_info(&inode, &name, entry) : rc;
}
static int guarded_stat_path(void *handle, const char *path, IfsFsEntryV2 *entry)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(stat_path(handle,path,entry));
}


static int list(void *handle, const char *path, uint32_t index,
                IfsFsEntryV2 *entry)
{
    Volume *v = mounted(handle);
    if (!v) return IFS_FS_INVALID;
    Inode inode;
    Dirent name;
    int rc = resolve(v, path, &inode, &name);
    if (rc != IFS_FS_OK) return rc;
    rc = find_entry(v, &inode, NULL, 0, index, &name);
    if (rc != IFS_FS_OK) return rc;
    rc = read_inode(v, name.number, &inode);
    return rc == IFS_FS_OK ? entry_info(&inode, &name, entry) : rc;
}
static int guarded_list(void *handle, const char *path, uint32_t index,
                IfsFsEntryV2 *entry)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(list(handle,path,index,entry));
}


static int read_file(void *handle, const char *path, uint64_t offset,
                     void *data, uint32_t bytes)
{
    Volume *v = mounted(handle);
    if (!v) return IFS_FS_INVALID;
    Inode inode;
    Dirent name;
    int rc = resolve(v, path, &inode, &name);
    if (rc != IFS_FS_OK) return rc;
    if (inode.type != FILE_REGULAR) return IFS_FS_INVALID;
    return read_data(v, &inode, offset, data, bytes);
}
static int guarded_read_file(void *handle, const char *path, uint64_t offset,
                     void *data, uint32_t bytes)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(read_file(handle,path,offset,data,bytes));
}


static int query_volume(void *handle, IfsFsVolumeInfoV2 *info)
{
    Volume *v = mounted(handle);
    if (!v || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *info = v->info;
    return IFS_FS_OK;
}
static int guarded_query_volume(void *handle, IfsFsVolumeInfoV2 *info)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(query_volume(handle,info));
}


static int label(void *handle, char *out, uint32_t capacity, uint32_t *bytes)
{
    Volume *v = mounted(handle);
    if (!v || !out || !bytes) return IFS_FS_INVALID;
    uint32_t length = 0;
    while (v->label[length]) ++length;
    if (capacity <= length) return IFS_FS_NO_SPACE;
    for (uint32_t i = 0; i <= length; ++i)
        out[i] = v->label[i];
    *bytes = length;
    return IFS_FS_OK;
}
static int guarded_label(void *handle, char *out, uint32_t capacity, uint32_t *bytes)
{
    if (bytes) *bytes=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(label(handle,out,capacity,bytes));
}


#if defined(IFS_EXT4_HOSTED_BUILD)
/* Map regular-file data using the same source-owned EXT4 extent and indirect
 * block decoder as CORE.READ. Coalesce physically adjacent logical blocks.
 * The V2 ABI has no defined sparse/unwritten extent flag; refuse such maps
 * rather than inventing a physical address or treating holes as free space. */
static int map_file(void *handle,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    Volume *v=mounted(handle);
    if(!v || !out || !capacity)return IFS_FS_INVALID;
    Inode inode;
    Dirent name;
    int rc=resolve(v,path,&inode,&name);
    if(rc!=IFS_FS_OK)return rc;
    if(inode.type!=FILE_REGULAR)return IFS_FS_INVALID;
    const uint32_t unit=v->info.block_bytes;
    const uint64_t blocks=inode.bytes/unit+(inode.bytes%unit!=0u);
    if(blocks>(UINT64_C(1)<<32))return IFS_FS_UNSUPPORTED;
    if(!blocks)return IFS_FS_END;
    uint64_t index=0u,logical_start=0u,physical_start=0u,previous=0u;
    uint32_t count=0u;
    for(uint64_t logical=0u;logical<blocks;++logical) {
        uint64_t physical=0u;
        if(v->host->cancelled && v->host->cancelled(v->host->context))
            return IFS_FS_BUSY;
        rc=extent(v,&inode,(uint32_t)logical,&physical);
        if(rc!=IFS_FS_OK)return rc;
        if(!physical)return IFS_FS_UNSUPPORTED;
        if(logical && physical!=previous+1u) {
            if(index>=first && count<capacity)
                out[count++]=(IfsFsExtentV2){logical_start*unit,
                    physical_start*unit,(logical-logical_start)*unit,0u,0u};
            ++index;
            logical_start=logical;
            physical_start=physical;
        } else if(!logical)physical_start=physical;
        previous=physical;
    }
    if(index>=first && count<capacity)
        out[count++]=(IfsFsExtentV2){logical_start*unit,physical_start*unit,
            inode.bytes-logical_start*unit,0u,0u};
    *written=count;
    return count?IFS_FS_OK:IFS_FS_END;
}
static int guarded_map_file(void *handle,const char *path,uint64_t first,
                            IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(map_file(handle,path,first,out,capacity,written));
}
/* A checksum-valid bitmap and matching descriptor counter can still
 * advertise live filesystem metadata as free. Verify the intersection of
 * every fixed metadata range with the current initialized group before
 * exposing its allocation bits. FLEX_BG can move a group's bitmap or inode
 * table outside that group's own allocation range. */
static int allocation_metadata_range(const Volume *v,const uint8_t *bitmap,
                                     uint64_t base,uint32_t span,
                                     uint64_t first,uint64_t count)
{
    if(!count || first<v->first || first>=v->blocks || count>v->blocks-first)
        return IFS_FS_CORRUPT;
    uint64_t at=first>base?first:base;
    const uint64_t end=first+count<base+span?first+count:base+span;
    for(;at<end;++at) {
        const uint32_t bit=(uint32_t)(at-base);
        if(!(bitmap[bit/8u]&(1u<<(bit%8u))))return IFS_FS_CORRUPT;
    }
    return IFS_FS_OK;
}
static int allocation_metadata(const Volume *v,uint32_t group,uint64_t base,
                               uint32_t span,const uint8_t *bitmap)
{
    const uint32_t unit=v->info.block_bytes;
    const uint64_t tables=((uint64_t)v->inodes_per_group*v->inode_bytes+
                           unit-1u)/unit;
    const uint64_t descriptors=(v->groups*v->desc_bytes+unit-1u)/unit;
    int rc;
    /* META_BG is rejected by open_volume; consequently the primary and
     * backup descriptor tables are contiguous after each superblock. */
    if(ifs_ext4_group_has_super_ex(
            (v->ro_compat&IFS_EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER)!=0u,
            (v->compat&0x200u)!=0u,v->backup_groups[0],v->backup_groups[1],group)) {
        rc=allocation_metadata_range(v,bitmap,base,span,base,
                                     1u+descriptors+v->reserved_gdt);
        if(rc!=IFS_FS_OK)return rc;
    }
    const bool flex=(v->incompat&IFS_EXT4_FEATURE_INCOMPAT_FLEX_BG)!=0u;
    const uint64_t start=flex?0u:group, end=flex?v->groups:(uint64_t)group+1u;
    for(uint64_t g=start;g<end;++g) {
        if(v->host->cancelled && v->host->cancelled(v->host->context))return IFS_FS_BUSY;
        uint8_t desc[MAX_BLOCK];
        rc=ifs_fs_host_read_bytes_v2(v->host,((uint64_t)v->first+1u)*unit+
                                    g*v->desc_bytes,desc,v->desc_bytes);
        if(rc!=IFS_FS_OK)return rc;
        if(v->checksum) {
            const uint16_t expected=u16(desc+30);
            desc[30]=desc[31]=0;
            if(g>UINT32_MAX ||
               (uint16_t)crc32c(crc_number(v->seed,(uint32_t)g),desc,v->desc_bytes)!=expected)
                return IFS_FS_CORRUPT;
        }
        uint64_t bb=u32(desc),ib=u32(desc+4),it=u32(desc+8);
        if(v->wide) {
            bb|=(uint64_t)u32(desc+32)<<32;
            ib|=(uint64_t)u32(desc+36)<<32;
            it|=(uint64_t)u32(desc+40)<<32;
        }
        if(!bb || !ib || !it || bb==ib ||
           (bb>=it && bb-it<tables) || (ib>=it && ib-it<tables))
            return IFS_FS_CORRUPT;
        if(!flex && (bb<base || bb-base>=span || ib<base || ib-base>=span ||
                     it<base || it-base>=span || tables>span-(it-base)))
            return IFS_FS_CORRUPT;
        rc=allocation_metadata_range(v,bitmap,base,span,bb,1u);
        if(rc!=IFS_FS_OK)return rc;
        rc=allocation_metadata_range(v,bitmap,base,span,ib,1u);
        if(rc!=IFS_FS_OK)return rc;
        rc=allocation_metadata_range(v,bitmap,base,span,it,tables);
        if(rc!=IFS_FS_OK)return rc;
    }
    return IFS_FS_OK;
}

/* Allocation is a separate physical address space from file extent maps.
 * A page covers at most one native block group. Validate the descriptor,
 * bitmap checksum where present, and the group's free-block count before
 * reporting any bitmap-derived classification. Lazy, uninitialised groups
 * are explicitly UNKNOWN, not guessed free. */
static int map_allocation(void *handle,uint64_t first,IfsFsAllocationRangeV2 *out,
                          uint32_t capacity,uint32_t *written)
{
    Volume *v=mounted(handle);
    if(!v || !out || !capacity || first>v->host->volume_bytes)return IFS_FS_INVALID;
    if(first==v->host->volume_bytes)return IFS_FS_END;
    if(v->host->cancelled && v->host->cancelled(v->host->context))return IFS_FS_BUSY;
    const uint32_t unit=v->info.block_bytes, bpg=v->blocks_per_group;
    const uint64_t begin=(uint64_t)v->first*unit, end=v->blocks*unit;
    if(first<begin || first>=end){
        uint64_t limit=first<begin?begin:v->host->volume_bytes;
        out[0]=(IfsFsAllocationRangeV2){first,limit-first,IFS_FS_SPACE_RESERVED,0u};
        *written=1u;return IFS_FS_OK;
    }
    if(!bpg || bpg>unit*8u || bpg%8u)return IFS_FS_CORRUPT;
    const uint64_t group64=(first/unit-v->first)/bpg;
    if(group64>UINT32_MAX)return IFS_FS_UNSUPPORTED;
    const uint32_t group=(uint32_t)group64;
    const uint64_t base=(uint64_t)v->first+group64*bpg;
    const uint32_t span=(uint32_t)(v->blocks-base<bpg?v->blocks-base:bpg);
    const uint64_t group_end=(base+span)*unit;
    uint8_t desc[MAX_BLOCK],bitmap[MAX_BLOCK];
    uint64_t address=((uint64_t)v->first+1u)*unit+group64*v->desc_bytes;
    int rc=ifs_fs_host_read_bytes_v2(v->host,address,desc,v->desc_bytes);
    if(rc!=IFS_FS_OK)return rc;
    if(v->checksum){
        const uint16_t expected=u16(desc+30);
        desc[30]=desc[31]=0;
        if((uint16_t)crc32c(crc_number(v->seed,group),desc,v->desc_bytes)!=expected)
            return IFS_FS_CORRUPT;
    }
    if(u16(desc+18)&2u){
        out[0]=(IfsFsAllocationRangeV2){first,group_end-first,IFS_FS_SPACE_UNKNOWN,0u};
        *written=1u;return IFS_FS_OK;
    }
    uint64_t block=u32(desc);
    uint32_t expected_free=u16(desc+12);
    if(v->wide){
        block|=(uint64_t)u32(desc+32)<<32;
        expected_free|=(uint32_t)u16(desc+44)<<16;
    }
    if(!block || block>=v->blocks || expected_free>span)return IFS_FS_CORRUPT;
    rc=ifs_fs_host_read_bytes_v2(v->host,block*unit,bitmap,unit);
    if(rc!=IFS_FS_OK)return rc;
    if(v->checksum){
        uint32_t expected=u16(desc+24);
        uint32_t actual=crc32c(v->seed,bitmap,bpg/8u);
        if(v->desc_bytes>=64u)expected|=(uint32_t)u16(desc+56)<<16;
        else actual&=UINT16_MAX;
        if(expected!=actual)return IFS_FS_CORRUPT;
    }
    uint32_t free_blocks=0;
    for(uint32_t i=0;i<span;++i)
        if(!(bitmap[i/8u]&(1u<<(i%8u))))++free_blocks;
    if(free_blocks!=expected_free)return IFS_FS_CORRUPT;
    rc=allocation_metadata(v,group,base,span,bitmap);
    if(rc!=IFS_FS_OK)return rc;
    uint32_t count=0;
    uint64_t cursor=first;
    while(cursor<group_end && count<capacity){
        uint32_t bit=(uint32_t)(cursor/unit-base);
        bool used=(bitmap[bit/8u]&(1u<<(bit%8u)))!=0u;
        uint32_t next=bit+1u;
        while(next<span && ((bitmap[next/8u]&(1u<<(next%8u)))!=0u)==used)++next;
        const uint64_t limit=(base+next)*unit;
        out[count++]=(IfsFsAllocationRangeV2){cursor,limit-cursor,
            used?IFS_FS_SPACE_ALLOCATED:IFS_FS_SPACE_FREE,0u};
        cursor=limit;
    }
    *written=count;return IFS_FS_OK;
}
static int guarded_map_allocation(void *handle,uint64_t first,IfsFsAllocationRangeV2 *out,
                                  uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0;
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(map_allocation(handle,first,out,capacity,written));
}
static const IfsFsAllocationOpsV2 allocation_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(allocation_ops),IFS_FS_GROUP_ALLOCATION,0u,
             IFS_FS_ALLOCATION_MAP},
    .map=guarded_map_allocation
};
static const IfsFsDefragOpsV2 defrag_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(defrag_ops),IFS_FS_GROUP_DEFRAG,0u,
             IFS_FS_DEFRAG_MAP},
    .map=guarded_map_file
};
#endif
static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(core), IFS_FS_GROUP_CORE, 0,
               IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT | IFS_FS_CORE_STAT |
               IFS_FS_CORE_LIST | IFS_FS_CORE_READ},
    .probe = guarded_probe, .mount = guarded_mount, .unmount = guarded_unmount, .stat = guarded_stat_path,
    .directory_entry = guarded_list, .read = guarded_read_file
};

static int metadata_get(void *handle, const char *path,
                        IfsFsMetadataV2 *metadata)
{
    Volume *v = mounted(handle);
    Inode inode;
    Dirent name;
    int rc;
    if (!v || !metadata ||
        metadata->structure_bytes < sizeof(*metadata))
        return IFS_FS_INVALID;
    rc = resolve(v, path, &inode, &name);
    if (rc != IFS_FS_OK)
        return rc;
    *metadata = (IfsFsMetadataV2){0};
    metadata->structure_bytes = sizeof(*metadata);
    metadata->mask = IFS_FS_META_GET_MODE;
    metadata->mode = inode.mode;
    metadata->owner = inode.owner;
    metadata->group = inode.group;
    return IFS_FS_OK;
}
static int guarded_metadata_get(void *handle, const char *path,
                        IfsFsMetadataV2 *metadata)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(metadata_get(handle,path,metadata));
}


static const IfsFsMetadataOpsV2 metadata_ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(metadata_ops),
               IFS_FS_GROUP_METADATA, 0, IFS_FS_META_GET_MODE},
    .get = guarded_metadata_get
};

#if defined(IFS_EXT4_HOSTED_BUILD)
/* Native EXT4 empty-volume formatting. The filesystem owns every on-disk
 * record; the caller provides only the exclusive, durable Common V2 lease.
 * This initial profile uses 4 KiB blocks, 256-byte inodes and complete
 * superblock/descriptor backups, extents and a clean internal JBD2 journal.
 * No pathname, external formatter or kernel mount is used. */
enum { FMT_BLOCK=4096u, FMT_BPG=32768u, FMT_IPG=1024u,
       FMT_INODE_BYTES=256u, FMT_ITABLE=64u, FMT_META=68u, FMT_JOURNAL=1024u,
       FMT_JIND=FMT_META+1u, FMT_JSTART=FMT_META+2u };
static void fmt16(uint8_t *p,uint32_t x)
{ p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8); }
static void fmt32(uint8_t *p,uint32_t x)
{ fmt16(p,x);fmt16(p+2,x>>16); }
static void fmtbe32(uint8_t *p,uint32_t x)
{ p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x; }
static int fmt_write(const IfsFsHostRuntimeV2 *h,uint64_t at,
                     const void *data,uint32_t n)
{
    if(h->cancelled && h->cancelled(h->context))return IFS_FS_BUSY;
    return ifs_fs_host_write_bytes_v2(h,at,data,n);
}
static void fmt_bit(uint8_t *bitmap,uint32_t bit)
{ bitmap[bit/8u]|=(uint8_t)(1u<<(bit%8u)); }
static int format_ext4(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                        const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    /* Without stable device identities, conservatively require all local
     * sessions closed before modifying any volume. */
    for(size_t i=0;i<MAX_MOUNTS;++i)
        if(mounts[i].cookie)return IFS_FS_BUSY;
    if(variant!=4u || flags || h->logical_sector_bytes>FMT_BLOCK ||
       h->volume_bytes%FMT_BLOCK)return IFS_FS_UNSUPPORTED;
    const uint64_t count=h->volume_bytes/FMT_BLOCK;
    /* A one-block group descriptor table holds up to 128 entries.
     * Refuse a short final group instead of rounding off its capacity. */
    if(count<4096u || count>128ull*FMT_BPG)return IFS_FS_UNSUPPORTED;
    const uint32_t blocks=(uint32_t)count;
    const uint32_t groups=(blocks+FMT_BPG-1u)/FMT_BPG;
    const uint32_t tail=blocks-(groups-1u)*FMT_BPG;
    if(tail<=FMT_META+1u)return IFS_FS_UNSUPPORTED;
    const size_t label_bytes=label?strlen(label):0u;
    if(label_bytes>16u)return IFS_FS_INVALID;

    /* A new volume needs an independent identity; a missing entropy
     * callback must fail before the first destructive write, never
     * silently create another all-zero UUID. */
    uint8_t uuid[16]={0};
    const int entropy_status=ifs_fs_host_random_bytes_v2(h,uuid,sizeof(uuid));
    if(entropy_status!=IFS_FS_OK)return entropy_status;
    uuid[6]=(uint8_t)((uuid[6]&15u)|64u);
    uuid[8]=(uint8_t)((uuid[8]&63u)|128u);
    uint32_t now=0u;
    if(h->realtime_ns) {
        const uint64_t seconds=h->realtime_ns(h->context)/1000000000ull;
        if(seconds<=UINT32_MAX)now=(uint32_t)seconds;
    }

    uint8_t block[FMT_BLOCK]={0},desc[FMT_BLOCK]={0},sb[1024]={0};
    uint32_t free_blocks=0u;
    int rc;
    /* Clear the primary signature first. No new valid superblock is
     * published until all allocation metadata and root data are durable. */
    for(uint32_t g=0u;g<groups;++g) {
        rc=fmt_write(h,(uint64_t)g*FMT_BPG*FMT_BLOCK,block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
    }
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;

    for(uint32_t g=0u;g<groups;++g) {
        const uint32_t base=g*FMT_BPG;
        const uint32_t span=g+1u==groups?tail:FMT_BPG;
        const uint32_t used=FMT_META+(g==0u?2u+FMT_JOURNAL:0u);
        const uint32_t available=span-used;
        uint8_t *d=desc+g*32u;
        fmt32(d,base+2u);fmt32(d+4,base+3u);fmt32(d+8,base+4u);
        fmt16(d+12,available);fmt16(d+14,FMT_IPG-(g==0u?10u:0u));
        fmt16(d+16,g==0u?1u:0u);
        free_blocks+=available;
        /* Mark metadata and nonexistent tail blocks allocated. */
        memset(block,0,sizeof(block));
        for(uint32_t bit=0u;bit<used;++bit)fmt_bit(block,bit);
        for(uint32_t bit=span;bit<FMT_BPG;++bit)fmt_bit(block,bit);
        rc=fmt_write(h,(uint64_t)(base+2u)*FMT_BLOCK,block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
        /* Inode bits 0..9 are reserved in group zero (root is inode 2).
         * Bits beyond the declared inode count are unavailable. */
        memset(block,0xff,sizeof(block));
        memset(block,0,FMT_IPG/8u);
        if(!g){block[0]=0xffu;block[1]=3u;}
        rc=fmt_write(h,(uint64_t)(base+3u)*FMT_BLOCK,block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
        for(uint32_t i=0u;i<FMT_ITABLE;++i) {
            memset(block,0,sizeof(block));
            if(!g && !i) {
                uint8_t *inode=block+FMT_INODE_BYTES;
                fmt16(inode,0040755u);
                fmt32(inode+4,FMT_BLOCK);
                fmt32(inode+8,now);fmt32(inode+12,now);fmt32(inode+16,now);
                fmt16(inode+26,2u);fmt32(inode+28,FMT_BLOCK/512u);
                /* Root directory uses a real one-record extent tree.
                 * The journal retains valid legacy block pointers, a
                 * layout explicitly supported on extents-capable EXT4. */
                fmt32(inode+32,EXTENTS_FLAG);
                fmt16(inode+40,EXTENT_MAGIC);fmt16(inode+42,1u);
                fmt16(inode+44,4u);
                fmt32(inode+52,0u);fmt16(inode+56,1u);
                fmt32(inode+60,FMT_META);
                /* Inode eight is the reserved, fully allocated internal
                 * journal. A single indirect block holds its remaining
                 * 1012 block addresses. It has no directory entry. */
                uint8_t *journal=block+7u*FMT_INODE_BYTES;
                fmt16(journal,0100600u);
                fmt32(journal+4,FMT_JOURNAL*FMT_BLOCK);
                fmt32(journal+8,now);fmt32(journal+12,now);
                fmt32(journal+16,now);fmt16(journal+26,1u);
                fmt32(journal+28,(FMT_JOURNAL+1u)*(FMT_BLOCK/512u));
                for(uint32_t j=0u;j<12u;++j)
                    fmt32(journal+40u+4u*j,FMT_JSTART+j);
                fmt32(journal+88,FMT_JIND);
            }
            rc=fmt_write(h,(uint64_t)(base+4u+i)*FMT_BLOCK,block,sizeof(block));
            if(rc!=IFS_FS_OK)return rc;
        }
    }
    memset(block,0,sizeof(block));
    fmt32(block,2u);fmt16(block+4,12u);block[6]=1u;block[7]=2u;block[8]='.';
    fmt32(block+12,2u);fmt16(block+16,FMT_BLOCK-12u);
    block[18]=2u;block[19]=2u;block[20]='.';block[21]='.';
    rc=fmt_write(h,(uint64_t)FMT_META*FMT_BLOCK,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    /* The journal is initially empty: clear every log block, populate its
     * indirect map, then write a version-two JBD2 superblock in big endian.
     * A zero start field represents a clean journal, not an unplayed log. */
    memset(block,0,sizeof(block));
    for(uint32_t j=0u;j<FMT_JOURNAL-12u;++j)
        fmt32(block+4u*j,FMT_JSTART+12u+j);
    rc=fmt_write(h,(uint64_t)FMT_JIND*FMT_BLOCK,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    memset(block,0,sizeof(block));
    for(uint32_t j=1u;j<FMT_JOURNAL;++j) {
        rc=fmt_write(h,(uint64_t)(FMT_JSTART+j)*FMT_BLOCK,block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
    }
    fmtbe32(block,0xc03b3998u); /* JBD magic */
    fmtbe32(block+4,4u);       /* V2 journal superblock */
    fmtbe32(block+12,FMT_BLOCK);fmtbe32(block+16,FMT_JOURNAL);
    fmtbe32(block+20,1u);fmtbe32(block+24,1u);
    memcpy(block+48,uuid,sizeof(uuid));fmtbe32(block+64,1u);
    rc=fmt_write(h,(uint64_t)FMT_JSTART*FMT_BLOCK,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    for(uint32_t g=0u;g<groups;++g) {
        rc=fmt_write(h,((uint64_t)g*FMT_BPG+1u)*FMT_BLOCK,desc,sizeof(desc));
        if(rc!=IFS_FS_OK)return rc;
    }
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;

    fmt32(sb,groups*FMT_IPG);fmt32(sb+4,blocks);
    fmt32(sb+12,free_blocks);fmt32(sb+16,groups*FMT_IPG-10u);
    fmt32(sb+24,2u);fmt32(sb+28,2u);
    fmt32(sb+32,FMT_BPG);fmt32(sb+36,FMT_BPG);fmt32(sb+40,FMT_IPG);
    fmt32(sb+48,now);fmt16(sb+54,0xffffu);
    fmt16(sb+56,0xef53u);fmt16(sb+58,1u);fmt16(sb+60,1u);
    fmt32(sb+64,now);fmt32(sb+76,1u);
    fmt32(sb+84,11u);fmt16(sb+88,FMT_INODE_BYTES);
    fmt32(sb+92,4u); /* HAS_JOURNAL */
    fmt32(sb+224,8u); /* internal journal inode */
    sb[253]=1u; /* journal inode block-map backup type */
    for(uint32_t j=0u;j<12u;++j)fmt32(sb+268u+4u*j,FMT_JSTART+j);
    fmt32(sb+268u+4u*12u,FMT_JIND);
    fmt32(sb+268u+4u*16u,FMT_JOURNAL*FMT_BLOCK);
    fmt32(sb+96,0x42u); /* extents and directory file-type features */
    fmt32(sb+100,2u); /* large-file read-only compatibility feature */
    memcpy(sb+104,uuid,sizeof(uuid));
    if(label_bytes)memcpy(sb+120,label,label_bytes);
    fmt16(sb+254,32u);
    for(uint32_t g=1u;g<groups;++g) {
        fmt16(sb+90,g);
        rc=fmt_write(h,(uint64_t)g*FMT_BPG*FMT_BLOCK,sb,sizeof(sb));
        if(rc!=IFS_FS_OK)return rc;
    }
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    fmt16(sb+90,0u);
    rc=fmt_write(h,1024u,sb,sizeof(sb));
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0) {
        memset(sb,0,sizeof(sb));
        (void)ifs_fs_host_write_bytes_v2(h,1024u,sb,sizeof(sb));
        (void)h->flush(h->context);
        return IFS_FS_IO_ERROR;
    }
    uint32_t observed=0u;
    rc=probe(h,&observed);
    return rc!=IFS_FS_OK?rc:observed==4u?IFS_FS_OK:IFS_FS_CORRUPT;
}
static int guarded_format_ext4(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                        const char *label,uint32_t flags)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(format_ext4(h,variant,label,flags));
}

#endif /* hosted format */

static const IfsFsVolumeOpsV2 ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(ops), IFS_FS_GROUP_VOLUME, 0,
               IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_LABEL_GET
#if defined(IFS_EXT4_HOSTED_BUILD)
               | IFS_FS_VOLUME_FORMAT
#endif
    },
    .query = guarded_query_volume, .label = guarded_label
#if defined(IFS_EXT4_HOSTED_BUILD)
    , .format = guarded_format_ext4
#endif
};

static const IfsFsGroupHeaderV2 *query(uint32_t group, uint32_t lo, uint32_t hi)
{
    if (lo > IFS_FS_GROUP_ABI_V2 || hi < IFS_FS_GROUP_ABI_V2)
        return NULL;
    if (group == IFS_FS_GROUP_CORE)
        return &core.header;
    if (group == IFS_FS_GROUP_VOLUME)
        return &ops.header;
    if (group == IFS_FS_GROUP_METADATA)
        return &metadata_ops.header;
#if defined(IFS_EXT4_HOSTED_BUILD)
    if(group==IFS_FS_GROUP_DEFRAG)return &defrag_ops.header;
    if(group==IFS_FS_GROUP_ALLOCATION)return &allocation_ops.header;
#endif
    return NULL;
}

__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    static const IfsFsModuleApiV2 api = {
        .abi_version = IFS_FS_COMMON_ABI_V2, .structure_bytes = sizeof(api),
        .format_id = "ext4",
        .runtime_profiles =
#if defined(IFS_EXT4_HOSTED_BUILD)
            IFS_FS_PROFILE_HOSTED,
#else
            IFS_FS_PROFILE_FREESTANDING | IFS_FS_PROFILE_FIRMWARE,
#endif
        .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
                      IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME) |
                      IFS_FS_GROUP_BIT(IFS_FS_GROUP_METADATA)
#if defined(IFS_EXT4_HOSTED_BUILD)
                      |IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)
                      |IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION)
#endif
                      ,
        .query_group = query
    };
    return &api;
}

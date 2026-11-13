/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bounded EXT4 boot reader using only Common V2 host services. */
#include <stdbool.h>
#include <infiltratr/filesystem_module_v2.h>
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
    uint64_t blocks;
    uint32_t first, inodes, inodes_per_group, seed;
    uint16_t inode_bytes, desc_bytes;
    bool wide, checksum, filetype;
    char label[17];
} Volume;

typedef struct {
    uint64_t bytes;
    uint32_t flags, seed;
    uint16_t type;
    uint8_t map[60];
} Inode;

typedef struct {
    uint32_t number, length;
    char name[256];
} Dirent;

static Volume volume;
static uintptr_t generation, active;

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
    v->first = first;
    v->inodes = inodes;
    v->inodes_per_group = ipg;
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
    inode->type = u16(raw) & FILE_TYPE;
    inode->flags = u32(raw + 32);
    inode->bytes = u32(raw + 4) | ((uint64_t)u32(raw + 108) << 32);
    if (inode->flags & UNSUPPORTED_INODE_FLAGS)
        return IFS_FS_UNSUPPORTED;
    for (unsigned i = 0; i < sizeof(inode->map); ++i)
        inode->map[i] = raw[40 + i];
    return IFS_FS_OK;
}

/* The inode stores the root extent node. Children occupy whole blocks.
 * A sparse or unwritten logical block maps to physical block zero. */
static int extent(const Volume *v, const Inode *inode, uint32_t logical, uint64_t *physical)
{
    uint8_t block[MAX_BLOCK];
    const uint8_t *node = inode->map;
    uint32_t bytes = sizeof(inode->map);
    uint16_t depth = u16(node + 6);
    if (!(inode->flags & EXTENTS_FLAG))
        return IFS_FS_UNSUPPORTED;
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
    if (!entry || entry->structure_bytes < sizeof(*entry) ||
        !entry->name || entry->name_capacity <= name->length)
        return IFS_FS_INVALID;
    switch (inode->type) {
    case FILE_REGULAR: entry->type = IFS_FS_ENTRY_FILE; break;
    case FILE_DIRECTORY: entry->type = IFS_FS_ENTRY_DIRECTORY; break;
    case FILE_LINK: entry->type = IFS_FS_ENTRY_LINK; break;
    default: return IFS_FS_UNSUPPORTED;
    }
    entry->size_bytes = inode->type == FILE_DIRECTORY ? 0 : inode->bytes;
    entry->attributes = 0;
    entry->name_bytes = name->length;
    for (uint32_t i = 0; i <= name->length; ++i)
        entry->name[i] = name->name[i];
    return IFS_FS_OK;
}

static bool mounted(void *handle)
{
    return active && (uintptr_t)handle == active;
}

static int probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    if (!variant)
        return IFS_FS_INVALID;
    Volume candidate;
    int rc = open_volume(&candidate, host);
    *variant = rc == IFS_FS_OK ? 4u : 0u;
    return rc;
}

static int mount(const IfsFsHostRuntimeV2 *host, void **handle, IfsFsVolumeInfoV2 *info)
{
    if (!handle || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *handle = NULL;
    if (!ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    if (!(host->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
    if (active || generation == UINTPTR_MAX)
        return IFS_FS_BUSY;
    int rc = open_volume(&volume, host);
    if (rc != IFS_FS_OK)
        return rc;
    Inode root;
    rc = read_inode(&volume, ROOT, &root);
    if (rc != IFS_FS_OK)
        return rc;
    if (root.type != FILE_DIRECTORY)
        return IFS_FS_CORRUPT;
    active = ++generation;
    *handle = (void *)active;
    *info = volume.info;
    return IFS_FS_OK;
}

static int unmount(void *handle)
{
    if (!mounted(handle))
        return IFS_FS_INVALID;
    active = 0;
    volume.host = NULL;
    return IFS_FS_OK;
}

static int stat_path(void *handle, const char *path, IfsFsEntryV2 *entry)
{
    if (!mounted(handle))
        return IFS_FS_INVALID;
    Inode inode;
    Dirent name;
    int rc = resolve(&volume, path, &inode, &name);
    return rc == IFS_FS_OK ? entry_info(&inode, &name, entry) : rc;
}

static int list(void *handle, const char *path, uint32_t index, IfsFsEntryV2 *entry)
{
    if (!mounted(handle))
        return IFS_FS_INVALID;
    Inode inode;
    Dirent name;
    int rc = resolve(&volume, path, &inode, &name);
    if (rc != IFS_FS_OK)
        return rc;
    rc = find_entry(&volume, &inode, NULL, 0, index, &name);
    if (rc != IFS_FS_OK)
        return rc;
    rc = read_inode(&volume, name.number, &inode);
    return rc == IFS_FS_OK ? entry_info(&inode, &name, entry) : rc;
}

static int read_file(void *handle, const char *path, uint64_t offset, void *data, uint32_t bytes)
{
    if (!mounted(handle))
        return IFS_FS_INVALID;
    Inode inode;
    Dirent name;
    int rc = resolve(&volume, path, &inode, &name);
    if (rc != IFS_FS_OK)
        return rc;
    if (inode.type != FILE_REGULAR)
        return IFS_FS_INVALID;
    return read_data(&volume, &inode, offset, data, bytes);
}

static int query_volume(void *handle, IfsFsVolumeInfoV2 *info)
{
    if (!mounted(handle) || !info || info->structure_bytes < sizeof(*info))
        return IFS_FS_INVALID;
    *info = volume.info;
    return IFS_FS_OK;
}

static int label(void *handle, char *out, uint32_t capacity, uint32_t *bytes)
{
    if (!mounted(handle) || !out || !bytes)
        return IFS_FS_INVALID;
    uint32_t length = 0;
    while (volume.label[length])
        ++length;
    if (capacity <= length)
        return IFS_FS_INVALID;
    for (uint32_t i = 0; i <= length; ++i)
        out[i] = volume.label[i];
    *bytes = length;
    return IFS_FS_OK;
}

static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(core), IFS_FS_GROUP_CORE, 0,
               IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT | IFS_FS_CORE_STAT |
               IFS_FS_CORE_LIST | IFS_FS_CORE_READ},
    .probe = probe, .mount = mount, .unmount = unmount, .stat = stat_path,
    .directory_entry = list, .read = read_file
};

static const IfsFsVolumeOpsV2 ops = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(ops), IFS_FS_GROUP_VOLUME, 0,
               IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_LABEL_GET},
    .query = query_volume, .label = label
};

static const IfsFsGroupHeaderV2 *query(uint32_t group, uint32_t lo, uint32_t hi)
{
    if (lo > IFS_FS_GROUP_ABI_V2 || hi < IFS_FS_GROUP_ABI_V2)
        return NULL;
    if (group == IFS_FS_GROUP_CORE)
        return &core.header;
    if (group == IFS_FS_GROUP_VOLUME)
        return &ops.header;
    return NULL;
}

__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    static const IfsFsModuleApiV2 api = {
        .abi_version = IFS_FS_COMMON_ABI_V2, .structure_bytes = sizeof(api),
        .format_id = "ext4",
        .runtime_profiles = IFS_FS_PROFILE_HOSTED | IFS_FS_PROFILE_FREESTANDING |
                            IFS_FS_PROFILE_FIRMWARE,
        .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) | IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
        .query_group = query
    };
    return &api;
}

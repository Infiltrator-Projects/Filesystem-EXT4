/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded Common V2 EXT4 identification, validated by the canonical EXT4
 * format core. Existing V1 native operations remain during consumer cutover.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "../native/filesystems/ext4/core/ext4_core.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
static uint16_t le16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant)
{
    uint8_t *raw;
    const uint8_t *sb;
    uint32_t sector, span, off, shift, block_bytes, incompat, ro;
    uint32_t blocks, groups, inodes;
    uint16_t inode_bytes, desc_bytes;
    int result = IFS_FS_CORRUPT;
    if (!variant || !ifs_fs_host_v2_valid(h)) return IFS_FS_INVALID;
    sector = h->logical_sector_bytes;
    if (h->volume_bytes < 4096u || sector > h->volume_bytes)
        return IFS_FS_NOT_FOUND;
    if (h->maximum_transfer_bytes < sector) return IFS_FS_INVALID;
    span = sector > 4096u ? sector : 4096u;
    if (span > h->volume_bytes) return IFS_FS_NOT_FOUND;
    raw = h->allocate(h->context, span);
    if (!raw) return IFS_FS_NO_MEMORY;
    for (off = 0; off < span; off += sector)
        if (h->read_at(h->context, off, raw + off, sector)) {
            result = IFS_FS_IO_ERROR;
            goto done;
        }
    sb = raw + 1024u;
    if (le16(sb + 56) != 0xef53u) {
        result = IFS_FS_NOT_FOUND;
        goto done;
    }
    incompat = le32(sb + 96);
    ro = le32(sb + 100);
    /* Do not accept journal-only EXT3 media as independent EXT4. */
    if (!(incompat & (0x0040u | 0x0080u | 0x0200u | 0x2000u))) {
        result = IFS_FS_NOT_FOUND;
        goto done;
    }
    shift = le32(sb + 24);
    if (shift > 6u) goto done;
    block_bytes = 1024u << shift;
    blocks = le32(sb + 4);
    groups = le32(sb + 32);
    inodes = le32(sb + 40);
    inode_bytes = le16(sb + 88);
    desc_bytes = le16(sb + 254);
    if (!desc_bytes) desc_bytes = 32u;
    if (!blocks || (uint64_t)blocks * block_bytes > h->volume_bytes ||
        ifs_ext4_unsupported_incompat_features(incompat) ||
        ifs_ext4_unsupported_ro_compat_features(ro) ||
        ifs_ext4_validate_inode_geometry(block_bytes, inode_bytes,
                                         le32(sb + 84)) != IFS_EXT4_INODE_GEOMETRY_OK ||
        ifs_ext4_validate_group_geometry(block_bytes, inode_bytes, desc_bytes,
                          !!(incompat & 0x0080u), groups, inodes)
           != IFS_EXT4_GROUP_GEOMETRY_OK)
        goto done;
    *variant = 4u;
    result = IFS_FS_OK;
done:
    h->release(h->context, raw);
    return result;
}
static const IfsFsCoreOpsV2 core = {
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,IFS_FS_CORE_PROBE},
    .probe=probe
};
static const IfsFsGroupHeaderV2 *query(uint32_t group,uint32_t lo,uint32_t hi)
{
    return group==IFS_FS_GROUP_CORE && lo<=IFS_FS_GROUP_ABI_V2 &&
           hi>=IFS_FS_GROUP_ABI_V2 ? &core.header : NULL;
}
static const IfsFsModuleApiV2 api = {
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="ext4",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{ return &api; }

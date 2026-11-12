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
static int inspect(const IfsFsHostRuntimeV2 *h,uint32_t *variant,
                   IfsFsVolumeInfoV2 *volume)
{
    uint8_t *raw;
    const uint8_t *sb;
    uint32_t sector, span, off, shift, block_bytes, incompat, ro;
    uint64_t blocks, free_blocks;
    uint32_t groups, inodes;
    uint16_t inode_bytes, desc_bytes;
    int result = IFS_FS_CORRUPT;
    if (!variant)return IFS_FS_INVALID;
    *variant=0u;
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
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
    free_blocks=le32(sb+12);
    /* Validate the high 64-bit EXT4 total/free block counts as well. */
    if(incompat & 0x0080u) {
        blocks|=(uint64_t)le32(sb+0x150u)<<32u;
        free_blocks|=(uint64_t)le32(sb+0x158u)<<32u;
    }
    groups = le32(sb + 32);
    inodes = le32(sb + 40);
    inode_bytes = le16(sb + 88);
    desc_bytes = le16(sb + 254);
    if (!desc_bytes) desc_bytes = 32u;
    if (!blocks || free_blocks>blocks ||
        blocks>h->volume_bytes/block_bytes ||
        ifs_ext4_unsupported_incompat_features(incompat) ||
        ifs_ext4_unsupported_ro_compat_features(ro) ||
        ifs_ext4_validate_inode_geometry(block_bytes, inode_bytes,
                                         le32(sb + 84)) != IFS_EXT4_INODE_GEOMETRY_OK ||
        ifs_ext4_validate_group_geometry(block_bytes, inode_bytes, desc_bytes,
                          !!(incompat & 0x0080u), groups, inodes)
           != IFS_EXT4_GROUP_GEOMETRY_OK)
        goto done;
    *variant=4u;
    if(volume) {
        *volume=(IfsFsVolumeInfoV2){
            .structure_bytes=sizeof(*volume),.variant=4u,
            .max_name_bytes=255u,.block_bytes=block_bytes,
            .total_bytes=blocks*block_bytes,
            .free_bytes=free_blocks*block_bytes
        };
    }
    result=IFS_FS_OK;
done:
    h->release(h->context, raw);
    return result;
}
static int probe(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{ return inspect(h,variant,NULL); }

/* Read-only Common V2 session: the canonical EXT4 core already validated
 * feature flags and geometry; this reports the superblock's RECORDED free
 * block count, not an independently scanned block bitmap. The host alone
 * owns media I/O and writable access is always rejected. */
static struct {
    uintptr_t generation,active;
    IfsFsVolumeInfoV2 info;
} session;
static int mounted(void *handle)
{ return session.active && (uintptr_t)handle==session.active; }
static int mount_volume(const IfsFsHostRuntimeV2 *h, void **volume,
                        IfsFsVolumeInfoV2 *info)
{
    IfsFsVolumeInfoV2 measured={0};
    uint32_t variant=0u;
    int rc;
    if(!volume || !info || info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    *volume=NULL;
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(!(h->capabilities & IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    if(session.active || session.generation==UINTPTR_MAX)return IFS_FS_BUSY;
    rc=inspect(h,&variant,&measured);
    if(rc!=IFS_FS_OK)return rc;
    session.info=measured;
    session.active=++session.generation;
    *info=measured;
    *volume=(void *)session.active;
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    if(!mounted(volume))return IFS_FS_INVALID;
    session.active=0u;
    session.info=(IfsFsVolumeInfoV2){0};
    return IFS_FS_OK;
}
static int query_volume(void *volume,IfsFsVolumeInfoV2 *info)
{
    if(!mounted(volume) || !info ||
       info->structure_bytes<sizeof(*info))return IFS_FS_INVALID;
    *info=session.info;
    return IFS_FS_OK;
}
static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY},
    .query=query_volume
};
static const IfsFsCoreOpsV2 core = {
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT},
    .probe=probe,
    .mount=mount_volume,
    .unmount=unmount_volume
};
static const IfsFsGroupHeaderV2 *query(uint32_t group,uint32_t lo,uint32_t hi)
{
    if(lo>IFS_FS_GROUP_ABI_V2 || hi<IFS_FS_GROUP_ABI_V2)return NULL;
    if(group==IFS_FS_GROUP_CORE)return &core.header;
    if(group==IFS_FS_GROUP_VOLUME)return &volume_ops.header;
    return NULL;
}
static const IfsFsModuleApiV2 api = {
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="ext4",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{ return &api; }

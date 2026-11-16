/* SPDX-License-Identifier: GPL-3.0-or-later
 * A magic number and plausible superblock are NOT proof of a mountable
 * namespace. The actual positive probe/mount/read test uses mkfs images.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
static uint8_t disk[1048576];
static int read_at(void *ctx,uint64_t at,void *out,uint32_t bytes)
{
    (void)ctx;
    if (bytes!=512 || at>sizeof(disk) || bytes>sizeof(disk)-at) return -1;
    memcpy(out,disk+(size_t)at,bytes);
    return 0;
}
static void *allocate(void *ctx,size_t bytes){(void)ctx;return malloc(bytes);}
static void release(void *ctx,void *ptr){(void)ctx;free(ptr);}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"EXT4 V2: %s\n",#x);return 1;}}while(0)
int main(void)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"ext4"));
    CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(core && core->probe && core->mount && core->unmount &&
          core->stat && core->directory_entry && core->read && !core->write);
    CHECK(core->header.capabilities==
          (IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
           IFS_FS_CORE_LIST|IFS_FS_CORE_READ));
    CHECK(vol && vol->query && vol->label);
    IfsFsHostRuntimeV2 host={0};
    host.abi_version=IFS_FS_HOST_ABI_V2;
    host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.volume_bytes=sizeof(disk);
    host.logical_sector_bytes=512;
    host.physical_sector_bytes=512;
    host.maximum_transfer_bytes=512;
    host.context=disk;
    host.read_at=read_at;
    host.allocate=allocate;
    host.release=release;
    uint32_t variant=99u;
    CHECK(core->probe(&host,&variant)==IFS_FS_NOT_FOUND && variant==0u);
    /* Even a valid EXT4 magic is not a valid filesystem layout. */
    disk[1024+56]=0x53;
    disk[1024+57]=0xef;
    disk[1024+96]=0x40;
    CHECK(core->probe(&host,&variant)!=IFS_FS_OK && variant==0u);
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *handle=(void *)1;
    CHECK(core->mount(&host,&handle,&info)!=IFS_FS_OK && handle==NULL);
    host.maximum_transfer_bytes=0;
    CHECK(core->probe(&host,&variant)==IFS_FS_INVALID);
    return 0;
}

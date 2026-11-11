/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
static uint8_t disk[1048576];
static int read_at(void *ctx,uint64_t p,void *out,uint32_t n) {
    (void)ctx;
    if (p>sizeof(disk) || n>sizeof(disk)-p || n!=512) return -1;
    memcpy(out,disk+(size_t)p,n);return 0;
}
static void *alloc(void *c,size_t n) { (void)c;return malloc(n); }
static void release(void *c,void *p) { (void)c;free(p); }
static void put16(uint8_t *p,uint16_t n) { p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8); }
static void put32(uint8_t *p,uint32_t n) { for(int i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i)); }
#define CHECK(e) do { if(!(e)) { fprintf(stderr,"EXT4 V2 failed: %s\n",#e);return 1; } } while(0)
int main(void) {
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"ext4"));
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    CHECK(core && core->probe && !core->mount && !core->write);
    IfsFsHostRuntimeV2 h={0};
    h.abi_version=IFS_FS_HOST_ABI_V2;h.structure_bytes=sizeof(h);
    h.capabilities=IFS_FS_HOST_READ_ONLY;h.volume_bytes=sizeof(disk);
    h.logical_sector_bytes=512;h.physical_sector_bytes=512;
    h.maximum_transfer_bytes=512;h.context=disk;
    h.read_at=read_at;h.allocate=alloc;h.release=release;
    uint32_t variant=0;
    CHECK(core->probe(&h,&variant)==IFS_FS_NOT_FOUND);
    uint8_t *sb=disk+1024;
    put16(sb+56,0xef53);put16(sb+88,128);put16(sb+254,32);
    put32(sb+4,512);put32(sb+20,1);put32(sb+24,0);
    put32(sb+32,128);put32(sb+36,128);put32(sb+40,64);
    put32(sb+84,11);put32(sb+92,4);put32(sb+96,0x42);put32(sb+100,1);
    CHECK(core->probe(&h,&variant)==IFS_FS_OK && variant==4);
    put32(sb+96,2);variant=0;
    CHECK(core->probe(&h,&variant)!=IFS_FS_OK && variant==0);
    put32(sb+96,0x42);put32(sb+4,UINT32_MAX);
    CHECK(core->probe(&h,&variant)==IFS_FS_CORRUPT);
    put32(sb+4,512);h.maximum_transfer_bytes=0;
    CHECK(core->probe(&h,&variant)==IFS_FS_INVALID);
    return 0;
}

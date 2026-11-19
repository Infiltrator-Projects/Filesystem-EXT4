/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real-image namespace tests for the production EXT4.so Common V2 export.
 * All reads use a bounded aligned host, not a Linux filesystem mount.
 */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
typedef struct {int fd;uint32_t sector;uint64_t bytes;} Image;
static int read_at(void *context,uint64_t offset,void *output,uint32_t bytes)
{
    Image *i=context;
    if (!i || (offset%i->sector) || (bytes%i->sector) || bytes>4096 ||
        offset>i->bytes || bytes>i->bytes-offset) return -1;
    uint32_t done=0;
    while(done<bytes) {
        ssize_t n=pread(i->fd,(char *)output+done,bytes-done,(off_t)(offset+done));
        if(n<=0)return -1;
        done+=(uint32_t)n;
    }
    return 0;
}
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"EXT4 native Common V2 at %d: %s\n",__LINE__,#x);return 1;}}while(0)
static IfsFsHostRuntimeV2 host(Image *image)
{
    IfsFsHostRuntimeV2 h={0};
    h.abi_version=IFS_FS_HOST_ABI_V2;
    h.structure_bytes=sizeof(h);
    h.capabilities=IFS_FS_HOST_READ_ONLY;
    h.volume_bytes=image->bytes;
    h.logical_sector_bytes=image->sector;
    h.physical_sector_bytes=image->sector;
    h.maximum_transfer_bytes=4096;
    h.context=image;
    h.read_at=read_at;
    h.allocate=allocate;
    h.release=release;
    return h;
}
int main(int argc,char **argv)
{
    CHECK(argc==2 || (argc==3 && strcmp(argv[2],"--corrupt-htree")==0));
    const int corrupt_htree=argc==3;
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"ext4"));
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsMetadataOpsV2 *metadata =
        (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_METADATA);
    CHECK(core && volume && core->stat && core->directory_entry && core->read);
    CHECK(metadata && metadata->get &&
          metadata->header.capabilities == IFS_FS_META_GET_MODE);
    struct stat st;
    CHECK(stat(argv[1],&st)==0 && st.st_size>0);
    Image images[2]={{.fd=-1,.sector=512,.bytes=(uint64_t)st.st_size},
                     {.fd=-1,.sector=4096,.bytes=(uint64_t)st.st_size}};
    for(unsigned i=0;i<2;++i) {
        images[i].fd=open(argv[1],O_RDONLY);
        CHECK(images[i].fd>=0);
    }
    IfsFsHostRuntimeV2 first=host(&images[0]), second=host(&images[1]);
    uint32_t variant=0;
    CHECK(core->probe(&first,&variant)==IFS_FS_OK && variant==4);
    CHECK(core->probe(&second,&variant)==IFS_FS_OK && variant==4);
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    void *a=NULL,*b=NULL,*c=NULL;
    CHECK(core->mount(&first,&a,&info)==IFS_FS_OK && a);
    CHECK(info.variant==4 && info.block_bytes==4096);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&second,&b,&info)==IFS_FS_OK && b && b!=a);
    char name[256]={0};
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),
                        .name=name,.name_capacity=sizeof(name)};
    if(corrupt_htree) {
        /* Checksummed HTree metadata is inspected by path traversal, not
         * at mount time. Independent clean files must remain readable. */
        CHECK(core->stat(a,"/hello.txt",&entry)==IFS_FS_OK);
        CHECK(core->stat(a,"/etc/fstab",&entry)==IFS_FS_CORRUPT);
        CHECK(core->stat(b,"/etc/fstab",&entry)==IFS_FS_CORRUPT);
        CHECK(core->unmount(a)==IFS_FS_OK);
        CHECK(core->unmount(b)==IFS_FS_OK);
        for(unsigned i=0;i<2;++i) close(images[i].fd);
        return 0;
    }
    CHECK(core->stat(a,"/hello.txt",&entry)==IFS_FS_OK);
    IfsFsMetadataV2 mode = { .structure_bytes = sizeof(mode) };
    CHECK(metadata->get(a, "/hello.txt", &mode) == IFS_FS_OK);
    CHECK(mode.mask == IFS_FS_META_GET_MODE);
    CHECK((mode.mode & 0170000u) == 0100000u);
    mode.structure_bytes = sizeof(mode);
    CHECK(metadata->get(b, "/sub", &mode) == IFS_FS_OK);
    CHECK((mode.mode & 0170000u) == 0040000u);
    CHECK(entry.type==IFS_FS_ENTRY_FILE);
    CHECK(strcmp(name,"hello.txt")==0);
    const char expected[]="Common V2 EXT4 shared-object namespace\n";
    CHECK(entry.size_bytes==sizeof(expected)-1);
    char data[sizeof(expected)]={0};
    CHECK(core->read(a,"/hello.txt",0,data,sizeof(expected)-1)==IFS_FS_OK);
    CHECK(memcmp(data,expected,sizeof(expected)-1)==0);
    memset(data,0,sizeof(data));
    CHECK(core->read(b,"/alias.txt",0,data,sizeof(expected)-1)==IFS_FS_OK);
    CHECK(memcmp(data,expected,sizeof(expected)-1)==0);

    /* Multi-block, e2fsck-indexed /etc reproduces the firmware failure:
     * INDEX_FL + metadata_csum + a regular file in an HTree leaf. */
    CHECK(core->stat(a,"/etc",&entry)==IFS_FS_OK);
    CHECK(entry.type==IFS_FS_ENTRY_DIRECTORY && entry.size_bytes==12288);
    CHECK(core->stat(b,"/etc/fstab",&entry)==IFS_FS_OK);
    const char fstab[]="UUID=11111111-2222-3333-4444-555555555555 / ext4 defaults 0 1\n";
    CHECK(entry.type==IFS_FS_ENTRY_FILE && entry.size_bytes==sizeof(fstab)-1);
    CHECK(strcmp(name,"fstab")==0);
    char fstab_data[sizeof(fstab)]={0};
    CHECK(core->read(a,"/etc/fstab",0,fstab_data,sizeof(fstab)-1)==IFS_FS_OK);
    CHECK(memcmp(fstab_data,fstab,sizeof(fstab)-1)==0);
    uint32_t etc_entries=0, fstab_entries=0;
    for(uint32_t n=0;n<500;++n) {
        entry.structure_bytes=sizeof(entry);
        int rc=core->directory_entry(b,"/etc",n,&entry);
        if(rc==IFS_FS_END) break;
        CHECK(rc==IFS_FS_OK);
        ++etc_entries;
        if(strcmp(name,"fstab")==0)++fstab_entries;
    }
    CHECK(etc_entries==241 && fstab_entries==1);
    int found=0;
    for(uint32_t n=0;n<64;++n) {
        entry.structure_bytes=sizeof(entry);
        int rc=core->directory_entry(b,"/",n,&entry);
        if(rc==IFS_FS_END)break;
        CHECK(rc==IFS_FS_OK);
        if(strcmp(name,"hello.txt")==0)++found;
    }
    CHECK(found==1);
    char label[32]={0};
    uint32_t label_len=0;
    CHECK(volume->label(b,label,sizeof(label),&label_len)==IFS_FS_OK);
    CHECK(strcmp(label,"EXT4ROOT")==0 && label_len==8);
    info.structure_bytes=sizeof(info);
    CHECK(volume->query(a,&info)==IFS_FS_OK && info.block_bytes==4096);
    CHECK(core->unmount(a)==IFS_FS_OK);
    CHECK(core->unmount(a)==IFS_FS_INVALID);
    CHECK(core->read(a,"/hello.txt",0,data,sizeof(expected)-1)==IFS_FS_INVALID);
    CHECK(core->read(b,"/hello.txt",0,data,sizeof(expected)-1)==IFS_FS_OK);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&first,&c,&info)==IFS_FS_OK && c && c!=a);
    CHECK(core->read(a,"/hello.txt",0,data,sizeof(expected)-1)==IFS_FS_INVALID);
    CHECK(core->unmount(b)==IFS_FS_OK);
    CHECK(core->unmount(c)==IFS_FS_OK);
    for(unsigned i=0;i<2;++i)close(images[i].fd);
    return 0;
}

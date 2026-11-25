/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real-image namespace tests for the production EXT4.so Common V2 export.
 * All reads use a bounded aligned host, not a Linux filesystem mount.
 */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
typedef struct {
    int fd;
    uint32_t sector;
    uint64_t bytes;
    int fail_reads;
} Image;
static const IfsFsCoreOpsV2 *unmount_during_read;
static void *active_volume;
static int observed_unmount;
static int read_at(void *context,uint64_t offset,void *output,uint32_t bytes)
{
    if (unmount_during_read) {
        const IfsFsCoreOpsV2 *ops=unmount_during_read;
        unmount_during_read=NULL;
        observed_unmount=ops->unmount(active_volume);
        if (observed_unmount!=IFS_FS_BUSY) return -1;
    }
    Image *i=context;
    if (!i || i->fail_reads || (offset%i->sector) ||
        (bytes%i->sector) || bytes>4096 ||
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
    CHECK(argc==2 || (argc==3 && strcmp(argv[2],"--corrupt-htree")==0) ||
          (argc==4 && strcmp(argv[2],"--legacy-blockmap")==0));
    const int corrupt_htree=argc==3;
    const int legacy=argc==4;
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    /* This exact test also loads the no-import freestanding reader, using
     * the same Common V2 host callbacks and real on-disk fixtures. */
    const char *firmware=getenv("EXT4_TEST_MODULE");
    void *firmware_library=NULL;
    if(firmware && firmware[0]) {
        firmware_library=dlopen(firmware,RTLD_NOW|RTLD_LOCAL);
        if(!firmware_library)
            fprintf(stderr,"EXT4 freestanding dlopen: %s\n",dlerror());
        CHECK(firmware_library!=NULL);
        void *symbol=dlsym(firmware_library,IFS_FS_COMMON_EXPORT_V2);
        IfsFsModuleApiEntryV2 get_api=NULL;
        CHECK(symbol && sizeof(symbol)==sizeof(get_api));
        memcpy(&get_api,&symbol,sizeof(get_api));
        CHECK(get_api!=NULL);
        api=get_api();
        CHECK(api && api->runtime_profiles==
              (IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE));
    } else {
        CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    }
    CHECK(ifs_fs_api_v2_valid(api,"ext4"));
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsMetadataOpsV2 *metadata =
        (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_METADATA);
    const IfsFsDefragOpsV2 *defrag=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    CHECK(firmware_library?!defrag:(defrag && defrag->header.capabilities==IFS_FS_DEFRAG_MAP &&
          defrag->map && !defrag->plan && !defrag->execute));
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
    /* The real V2 probe and mount must preserve host errors instead of
     * turning a missing media read into a filesystem signature failure. */
    images[0].fail_reads=1;
    CHECK(core->probe(&first,&variant)==IFS_FS_IO_ERROR);
    IfsFsVolumeInfoV2 failed_info={.structure_bytes=sizeof(failed_info)};
    void *failed_volume=NULL;
    CHECK(core->mount(&first,&failed_volume,&failed_info)==IFS_FS_IO_ERROR &&
          failed_volume==NULL);
    images[0].fail_reads=0;
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
    CHECK(core->stat(a,"/",&entry)==IFS_FS_OK &&
          entry.type==IFS_FS_ENTRY_DIRECTORY &&
          entry.name_bytes==1u && strcmp(name,"/")==0);
    {
        char narrow[1]={'X'};
        IfsFsEntryV2 short_root={.structure_bytes=sizeof(short_root),
                                 .name=narrow,.name_capacity=sizeof(narrow),
                                 .name_bytes=77u};
        CHECK(core->stat(a,"/",&short_root)==IFS_FS_NO_SPACE &&
              narrow[0]=='X' && short_root.name_bytes==77u);
    }
    if(legacy) {
        /* A real EXT4 volume can have extents in its superblock but keep
         * older direct/indirect inodes. Compare bytes from the image's
         * block-mapped inode to the original file at the boundaries of
         * direct, single-indirect and double-indirect mapping. */
        struct stat original;
        CHECK(stat(argv[3],&original)==0 && original.st_size>8200000);
        CHECK(core->stat(a,"/payload.bin",&entry)==IFS_FS_OK);
        CHECK(entry.size_bytes==(uint64_t)original.st_size);
        int fd=open(argv[3],O_RDONLY);
        CHECK(fd>=0);
        const uint64_t offsets[]={0u,45056u,49152u,65536u,
                                  4194304u,8192000u};
        for(unsigned k=0;k<sizeof(offsets)/sizeof(offsets[0]);++k) {
            uint8_t expected[4096],actual[4096];
            CHECK(pread(fd,expected,sizeof(expected),
                        (off_t)offsets[k])==(ssize_t)sizeof(expected));
            CHECK(core->read(a,"/payload.bin",offsets[k],actual,
                             sizeof(actual))==IFS_FS_OK);
            CHECK(memcmp(actual,expected,sizeof(actual))==0);
            CHECK(core->read(b,"/long-alias.bin",offsets[k],actual,
                             sizeof(actual))==IFS_FS_OK);
            CHECK(memcmp(actual,expected,sizeof(actual))==0);
        }
        if(defrag) {
            uint64_t logical=0u;
            for(uint64_t page=0u;;++page) {
                IfsFsExtentV2 e={0};
                uint32_t count=99u;
                int rc=defrag->map(a,"/payload.bin",page,&e,1u,&count);
                if(rc==IFS_FS_END) { CHECK(!count); break; }
                CHECK(rc==IFS_FS_OK && count==1u && e.logical_offset==logical &&
                      e.length && e.length<=(uint64_t)original.st_size-logical);
                uint8_t expected[4096],actual[4096];
                size_t n=e.length<sizeof(expected)?(size_t)e.length:sizeof(expected);
                CHECK(pread(fd,expected,n,(off_t)logical)==(ssize_t)n);
                CHECK(pread(images[0].fd,actual,n,(off_t)e.physical_offset)==(ssize_t)n);
                CHECK(!memcmp(expected,actual,n));
                logical+=e.length;
            }
            CHECK(logical==(uint64_t)original.st_size);
        }
        close(fd);
        CHECK(core->unmount(a)==IFS_FS_OK);
        CHECK(core->unmount(b)==IFS_FS_OK);
        for(unsigned i=0;i<2;++i)close(images[i].fd);
        if(firmware_library)CHECK(dlclose(firmware_library)==0);
        return 0;
    }
    if(corrupt_htree) {
        /* Checksummed HTree metadata is inspected by path traversal, not
         * at mount time. Independent clean files must remain readable. */
        CHECK(core->stat(a,"/hello.txt",&entry)==IFS_FS_OK);
        CHECK(core->stat(a,"/etc/fstab",&entry)==IFS_FS_CORRUPT);
        CHECK(core->stat(b,"/etc/fstab",&entry)==IFS_FS_CORRUPT);
        CHECK(core->unmount(a)==IFS_FS_OK);
        CHECK(core->unmount(b)==IFS_FS_OK);
        for(unsigned i=0;i<2;++i) close(images[i].fd);
        if(firmware_library)CHECK(dlclose(firmware_library)==0);
        return 0;
    }
    CHECK(core->stat(a,"/hello.txt",&entry)==IFS_FS_OK);
    /* A valid path with insufficient name space must return NO_SPACE and
     * leave the previously returned Common V2 entry fields unchanged. */
    {
        char insufficient[2]={0};
        IfsFsEntryV2 short_entry={
            .structure_bytes=sizeof(short_entry),
            .name=insufficient,
            .name_capacity=sizeof(insufficient),
            .type=0xabcdef01u,
            .size_bytes=UINT64_C(0x12345678),
            .name_bytes=777u
        };
        CHECK(core->stat(a,"/hello.txt",&short_entry)==IFS_FS_NO_SPACE);
        CHECK(short_entry.type==0xabcdef01u &&
              short_entry.size_bytes==UINT64_C(0x12345678) &&
              short_entry.name_bytes==777u);
        CHECK(core->directory_entry(b,"/etc",0,&short_entry)==IFS_FS_NO_SPACE);
        CHECK(short_entry.type==0xabcdef01u);
        short_entry.name_capacity=0u;
        CHECK(core->stat(a,"/hello.txt",&short_entry)==IFS_FS_NO_SPACE);
        short_entry.name_capacity=sizeof(insufficient);
        short_entry.structure_bytes=sizeof(short_entry)-1u;
        CHECK(core->stat(a,"/hello.txt",&short_entry)==IFS_FS_INVALID);
    }
    /* Once mounted, actual failing block reads must be propagated, never
     * misclassified as missing paths or corrupted directory entries. */
    images[0].fail_reads=1;
    CHECK(core->stat(a,"/hello.txt",&entry)==IFS_FS_IO_ERROR);
    char failed_data[4]={0};
    CHECK(core->read(a,"/hello.txt",0u,failed_data,1u)==IFS_FS_IO_ERROR);
    images[0].fail_reads=0;
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
    active_volume=a;
    unmount_during_read=core;
    observed_unmount=IFS_FS_INVALID;
    CHECK(core->read(a,"/hello.txt",0,data,sizeof(expected)-1)==IFS_FS_OK);
    CHECK(!unmount_during_read && observed_unmount==IFS_FS_BUSY);
    CHECK(memcmp(data,expected,sizeof(expected)-1)==0);
    if(defrag) {
        IfsFsExtentV2 e={0};
        uint32_t count=99u;
        CHECK(defrag->map(a,"/hello.txt",0u,&e,1u,&count)==IFS_FS_OK &&
              count==1u && e.logical_offset==0u && e.length==sizeof(expected)-1);
        CHECK(pread(images[0].fd,data,sizeof(expected)-1,(off_t)e.physical_offset)==
              (ssize_t)(sizeof(expected)-1));
        CHECK(!memcmp(data,expected,sizeof(expected)-1));
        CHECK(defrag->map(a,"/hello.txt",1u,&e,1u,&count)==IFS_FS_END && !count);
        CHECK(defrag->map(a,"/",0u,&e,1u,&count)==IFS_FS_INVALID && !count);
    }
    memset(data,0,sizeof(data));
    CHECK(core->read(b,"/alias.txt",0,data,sizeof(expected)-1)==IFS_FS_OK);
    CHECK(memcmp(data,expected,sizeof(expected)-1)==0);

    /* Multi-block, e2fsck-indexed /etc reproduces the firmware failure:
     * INDEX_FL + metadata_csum + a regular file in an HTree leaf. */
    CHECK(core->stat(a,"/etc",&entry)==IFS_FS_OK);
    CHECK(entry.type==IFS_FS_ENTRY_DIRECTORY && entry.size_bytes==12288);
    CHECK(core->stat(b,"/etc/fstab",&entry)==IFS_FS_OK);
    const char fstab[]="UUID=11111111-2222-3333-4444-555555555555 / ext4 defaults 0 1\n";
    CHECK(entry.type==IFS_FS_ENTRY_FILE && entry.size_bytes==665u);
    CHECK(strcmp(name,"fstab")==0);
    char fstab_data[665]={0};
    CHECK(core->read(a,"/etc/fstab",0,fstab_data,sizeof(fstab_data))==IFS_FS_OK);
    CHECK(memcmp(fstab_data,fstab,sizeof(fstab)-1)==0);
    for(unsigned j=sizeof(fstab)-1;j<sizeof(fstab_data);++j)
        CHECK(fstab_data[j]=='#');
    uint32_t etc_entries=0, fstab_entries=0;
    for(uint32_t n=0;n<500;++n) {
        entry.structure_bytes=sizeof(entry);
        int rc=core->directory_entry(b,"/etc",n,&entry);
        if(rc==IFS_FS_END) break;
        CHECK(rc==IFS_FS_OK);
        ++etc_entries;
        if(strcmp(name,"fstab")==0)++fstab_entries;
    }
    CHECK(etc_entries==171 && fstab_entries==1);
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
    {
        char small_label[8]={0};
        uint32_t needed=0u;
        CHECK(volume->label(b,small_label,sizeof(small_label),&needed)==
              IFS_FS_NO_SPACE);
        CHECK(volume->label(b,small_label,0u,&needed)==IFS_FS_NO_SPACE);
        CHECK(volume->label(b,NULL,16u,&needed)==IFS_FS_INVALID);
    }
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
    if(firmware_library)CHECK(dlclose(firmware_library)==0);
    return 0;
}

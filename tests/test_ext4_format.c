/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <infiltratr/filesystem_module_v2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/random.h>
#include <errno.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
typedef struct {int fd;uint64_t bytes;uint32_t sector;unsigned writes,flushes,fail_flush;} Media;
static int read_at(void *ctx,uint64_t at,void *p,uint32_t n)
{
    Media *m=ctx;
    if(at%m->sector || n%m->sector || n>m->sector || at>m->bytes || n>m->bytes-at)return -1;
    return pread(m->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int write_at(void *ctx,uint64_t at,const void *p,uint32_t n)
{
    Media *m=ctx;
    if(at%m->sector || n%m->sector || n>m->sector || at>m->bytes || n>m->bytes-at)return -1;
    ++m->writes;
    return pwrite(m->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int flush(void *ctx)
{
    Media *m=ctx;
    if(++m->flushes==m->fail_flush)return -1;
    return fsync(m->fd);
}
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
static int entropy(void *ctx,void *p,uint32_t n)
{(void)ctx;return getrandom(p,n,0)==(ssize_t)n?0:-1;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"EXT4 format line %d: %s\n",__LINE__,#x);goto done;}}while(0)
int main(int argc,char **argv)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    if(!ifs_fs_api_v2_valid(api,"ext4"))return 1;
    const IfsFsCoreOpsV2 *core=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(!core || !vol || !vol->format)return 1;
    char temp[]="/tmp/ifs-ext4-format.XXXXXX";
    if(argc!=1 && argc!=4)return 2;
    Media m={.bytes=64ull*1024u*1024u,.sector=512u};
    if(argc==4) {
        m.bytes=strtoull(argv[2],NULL,10)*1024u*1024u;
        m.sector=(uint32_t)strtoul(argv[3],NULL,10);
        m.fd=open(argv[1],O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW,0600);
    } else m.fd=mkstemp(temp);
    if(m.fd<0)return 2;
    if(argc==1)unlink(temp);
    int result=1;
    CHECK(ftruncate(m.fd,(off_t)m.bytes)==0);
    IfsFsHostRuntimeV2 h={
        .abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(h),
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
        .volume_bytes=m.bytes,.logical_sector_bytes=m.sector,
        .physical_sector_bytes=m.sector,.maximum_transfer_bytes=m.sector,
        .context=&m,.read_at=read_at,.write_at=write_at,.flush=flush,
        .allocate=allocate,.release=release,.random_bytes=entropy
    },ro=h;
    ro.capabilities=IFS_FS_HOST_READ_ONLY;ro.write_at=NULL;ro.flush=NULL;
    CHECK(vol->format(&ro,4u,"EXT4TEST",0u)==IFS_FS_ACCESS);
    CHECK(vol->format(&h,3u,"EXT4TEST",0u)==IFS_FS_UNSUPPORTED);
    CHECK(vol->format(&h,4u,"01234567890123456",0u)==IFS_FS_INVALID);
    CHECK(!m.writes && !m.flushes);
    if(argc==1) {
        m.fail_flush=2u;
        CHECK(vol->format(&h,4u,"EXT4TEST",0u)==IFS_FS_IO_ERROR);
        uint32_t rejected=0u;
        CHECK(core->probe(&ro,&rejected)!=IFS_FS_OK);
        m.fail_flush=0u;
    }
    CHECK(vol->format(&h,4u,"EXT4TEST",0u)==IFS_FS_OK);
    uint32_t v=0u;void *session=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    char name[256];
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,.name_capacity=sizeof(name)};
    CHECK(core->probe(&ro,&v)==IFS_FS_OK && v==4u);
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_OK && session);
    CHECK(info.variant==4u && info.block_bytes==4096u && info.total_bytes==m.bytes);
    CHECK(info.free_bytes>0u && info.free_bytes<info.total_bytes);
    CHECK(core->stat(session,"/",&entry)==IFS_FS_OK && entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->directory_entry(session,"/",0u,&entry)==IFS_FS_END);
    char label[32];uint32_t label_bytes=0u;
    CHECK(vol->label && vol->label(session,label,sizeof(label),&label_bytes)==IFS_FS_OK &&
          label_bytes==8u && !strcmp(label,"EXT4TEST"));
    CHECK(vol->format(&h,4u,"EXT4TEST",0u)==IFS_FS_BUSY);
    CHECK(core->unmount(session)==IFS_FS_OK);
    CHECK(core->mount(&h,&session,&info)==IFS_FS_ACCESS && !session);
    puts("EXT4 Common V2 native format, probe, root and mount PASS");
    result=0;
done:
    close(m.fd);
    if(result && argc==4)unlink(argv[1]);
    return result;
}

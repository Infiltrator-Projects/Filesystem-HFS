/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strict Common V2 classic-HFS probe regression with disposable byte arrays.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
#define SIZE (1024u*1024u)
static unsigned char media[SIZE];
static int fail_reads;
static uint32_t logical_sector=512u,maximum_read=512u;
static int rd(void *ctx,uint64_t at,void *dst,uint32_t n) {
    (void)ctx;
    if(fail_reads || (at%logical_sector) || (n%logical_sector) ||
       n>maximum_read || at>SIZE || n>SIZE-at)return -1;
    memcpy(dst,media+(size_t)at,n);return 0;
}
static void *alloc(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void drop(void *ctx,void *p){(void)ctx;free(p);}
static void be16(unsigned offset,uint16_t n) {
    media[offset]=(unsigned char)(n>>8);
    media[offset+1]=(unsigned char)n;
}
static void be32(unsigned offset,uint32_t n) {
    media[offset]=(unsigned char)(n>>24);
    media[offset+1]=(unsigned char)(n>>16);
    media[offset+2]=(unsigned char)(n>>8);
    media[offset+3]=(unsigned char)n;
}

/* An on-disk classic-HFS volume with a genuine catalog node, two data forks,
 * directory thread-free root and a nested directory. The catalog is read
 * through the production Common V2 module, never a private fixture decoder. */
static void catalog_record(unsigned at, uint32_t parent, const char *name,
                           uint32_t id, int directory,
                           uint16_t alloc_block, const char *payload)
{
    size_t length=strlen(name);
    media[at]=(unsigned char)(6u+length);
    be32(at+2u,parent);
    media[at+6u]=(unsigned char)length;
    memcpy(media+at+7u,name,length);
    unsigned data=at+((unsigned)(length+8u)&~1u);
    media[data]=(unsigned char)(directory?1u:2u);
    if(directory) {
        be32(data+6u,id);
    } else {
        size_t bytes=strlen(payload);
        be32(data+20u,id);
        be32(data+26u,(uint32_t)bytes);
        be32(data+30u,4096u);
        be16(data+74u,alloc_block);
        be16(data+76u,1u);
        memcpy(media+64u*512u+(unsigned)alloc_block*4096u,payload,bytes);
    }
}
static void make_catalog(void)
{
    const unsigned tree=64u*512u,leaf=tree+512u;
    /* drCTFlSize and drCTExtRec: catalogue fork is allocation block zero. */
    be32(1024u+146u,1024u);
    be16(1024u+150u,0u);
    be16(1024u+152u,1u);
    media[tree+8u]=1u; /* B-tree header node */
    be16(tree+10u,1u);
    be16(tree+14u,1u);  /* tree depth */
    be32(tree+16u,1u);  /* root node */
    be32(tree+20u,3u);  /* leaf records */
    be32(tree+24u,1u);  /* first leaf */
    be32(tree+28u,1u);  /* last leaf */
    be16(tree+32u,512u); /* node size */
    be16(tree+34u,37u); /* max classic HFS catalogue key */
    be32(tree+36u,2u); /* total nodes */
    media[leaf+8u]=0xffu; /* leaf */
    media[leaf+9u]=1u;   /* leaf level */
    be16(leaf+10u,3u);
    catalog_record(leaf+14u,2u,"kernel.img",16u,0,1u,"REAL-HFS-KERNEL");
    catalog_record(leaf+134u,2u,"Boot",17u,1,0u,"");
    catalog_record(leaf+216u,17u,"initramfs",18u,0,2u,"REAL-HFS-INITRD");
    be16(leaf+510u,14u);
    be16(leaf+508u,134u);
    be16(leaf+506u,216u);
    be16(leaf+504u,334u);
}
static uint64_t media_hash(void)
{
    uint64_t hash=UINT64_C(14695981039346656037);
    for(size_t i=0;i<SIZE;++i) {
        hash^=media[i];
        hash*=UINT64_C(1099511628211);
    }
    return hash;
}

#define CHECK(x) do {if(!(x)) {fprintf(stderr,"HFS V2 failure: %s line %d\n",#x,__LINE__);return 1;}}while(0)
/* Valid writable-host callbacks: provider must refuse before using them. */
static int forbidden_write(void *ctx,uint64_t at,const void *src,uint32_t n)
{ (void)ctx;(void)at;(void)src;(void)n;return -1; }
static int forbidden_flush(void *ctx)
{ (void)ctx;return -1; }
int main(void) {
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"hfs"));
    CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    CHECK(api->group_mask==(IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                            IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)));
    const IfsFsCoreOpsV2 *core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *ops=(const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(core && core->header.capabilities==(IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|
               IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ));
    CHECK(core->stat && core->directory_entry && core->read);
    CHECK(core->probe && core->mount && core->unmount && !core->write);
    CHECK(ops && ops->header.capabilities==IFS_FS_VOLUME_QUERY &&
          ops->query && !ops->format && !ops->repair);
    CHECK(!ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG));
    IfsFsHostRuntimeV2 host={0};
    host.abi_version=IFS_FS_HOST_ABI_V2;
    host.structure_bytes=sizeof(host);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.volume_bytes=SIZE;
    host.logical_sector_bytes=512u;
    host.physical_sector_bytes=512u;
    host.maximum_transfer_bytes=512u;
    host.read_at=rd;
    host.allocate=alloc;
    host.release=drop;
    CHECK(ifs_fs_host_v2_valid(&host));
    uint32_t variant=99u;
    CHECK(core->probe(&host,&variant)==IFS_FS_NOT_FOUND && variant==0u);
    be16(1024u,0x4244u); /* classic HFS */
    be16(1024u+14u,3u);  /* bitmap block */
    be16(1024u+18u,100u);
    be32(1024u+20u,4096u);
    be16(1024u+28u,64u); /* data begins after bitmap */
    be16(1024u+34u,40u);
    /* 60 used and 40 free blocks in an actual HFS MSB-first bitmap. */
    for(unsigned bit=0u;bit<60u;++bit)
        media[3u*512u+(bit>>3u)] |=
            (unsigned char)(0x80u>>(bit&7u));
    make_catalog();
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1u);
    media[3u*512u]^=0x80u; /* MDB unchanged: must reject disagreement */
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT);
    media[3u*512u]^=0x80u;
    CHECK(core->probe(&host,&variant)==IFS_FS_OK);
    IfsFsVolumeInfoV2 info={0},recheck={0};
    void *mounted=NULL,*stale=NULL;
    info.structure_bytes=sizeof(info);
    host.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;
    host.write_at=forbidden_write;host.flush=forbidden_flush;
    CHECK(ifs_fs_host_v2_valid(&host));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK);
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_ACCESS && !mounted);
    host.capabilities=IFS_FS_HOST_READ_ONLY;
    host.write_at=NULL;host.flush=NULL;
    CHECK(ifs_fs_host_v2_valid(&host));
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_OK && mounted);
    stale=mounted;
    CHECK(info.variant==1u && info.block_bytes==4096u &&
          info.total_bytes==409600u && info.free_bytes==163840u &&
          info.max_name_bytes==31u);
    {
        char name[128]={0},bytes[32]={0};
        IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,
                            .name_capacity=sizeof(name)};
        uint64_t before=media_hash();
        CHECK(core->stat(mounted,"/",&entry)==IFS_FS_OK &&
              entry.type==IFS_FS_ENTRY_DIRECTORY);
        CHECK(core->stat(mounted,"/kernel.img",&entry)==IFS_FS_OK &&
              entry.type==IFS_FS_ENTRY_FILE &&
              entry.size_bytes==strlen("REAL-HFS-KERNEL"));
        CHECK(core->directory_entry(mounted,"/",0u,&entry)==IFS_FS_OK &&
              !strcmp(name,"kernel.img"));
        CHECK(core->directory_entry(mounted,"/",1u,&entry)==IFS_FS_OK &&
              !strcmp(name,"Boot") && entry.type==IFS_FS_ENTRY_DIRECTORY);
        CHECK(core->directory_entry(mounted,"/",2u,&entry)==IFS_FS_END);
        CHECK(core->stat(mounted,"/Boot/initramfs",&entry)==IFS_FS_OK &&
              entry.type==IFS_FS_ENTRY_FILE);
        CHECK(core->read(mounted,"/Boot/initramfs",0u,bytes,15u)==IFS_FS_OK &&
              !memcmp(bytes,"REAL-HFS-INITRD",15u));
        CHECK(core->read(mounted,"/kernel.img",5u,bytes,3u)==IFS_FS_OK &&
              !memcmp(bytes,"HFS",3u));
        CHECK(core->stat(mounted,"/absent",&entry)==IFS_FS_NOT_FOUND);
        CHECK(core->read(mounted,"/Boot/../kernel.img",0u,bytes,1u)
              ==IFS_FS_INVALID);
        CHECK(media_hash()==before);
    }
    recheck.structure_bytes=sizeof(recheck);
    CHECK(ops->query(mounted,&recheck)==IFS_FS_OK &&
          recheck.free_bytes==info.free_bytes &&
          recheck.total_bytes==info.total_bytes);
    {
        void *parallel_volume=NULL;
        IfsFsVolumeInfoV2 parallel={.structure_bytes=sizeof(parallel)};
        CHECK(core->mount(&host,&parallel_volume,&parallel)==IFS_FS_OK &&
              parallel_volume && parallel_volume!=mounted);
        CHECK(parallel.total_bytes==info.total_bytes &&
              parallel.free_bytes==info.free_bytes);
        parallel.structure_bytes=sizeof(parallel);
        CHECK(ops->query(parallel_volume,&parallel)==IFS_FS_OK);
        CHECK(core->unmount(parallel_volume)==IFS_FS_OK);
        parallel.structure_bytes=sizeof(parallel);
        CHECK(ops->query(parallel_volume,&parallel)==IFS_FS_INVALID);
    }
    CHECK(core->unmount(mounted)==IFS_FS_OK);
    CHECK(core->unmount(stale)==IFS_FS_INVALID);
    recheck.structure_bytes=sizeof(recheck);
    CHECK(ops->query(stale,&recheck)==IFS_FS_INVALID);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_OK &&
          mounted && mounted!=stale);
    CHECK(ops->query(stale,&recheck)==IFS_FS_INVALID);
    CHECK(core->unmount(mounted)==IFS_FS_OK);
    /* On 4Kn media, the host must NEVER receive read_at(1024,512).
     * The same 512-byte MDB still begins at byte 1024. */
    host.logical_sector_bytes=4096u;
    host.physical_sector_bytes=4096u;
    host.maximum_transfer_bytes=4096u;
    logical_sector=4096u;maximum_read=4096u;
    CHECK(ifs_fs_host_v2_valid(&host));
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1u);
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_OK &&
          info.block_bytes==4096u && info.free_bytes==163840u);
    CHECK(core->unmount(mounted)==IFS_FS_OK);
    logical_sector=512u;maximum_read=512u;
    host.logical_sector_bytes=512u;
    host.physical_sector_bytes=512u;
    host.maximum_transfer_bytes=512u;
    be16(1024u+124u,0x482bu);
    CHECK(core->probe(&host,&variant)==IFS_FS_UNSUPPORTED && variant==0u);
    be16(1024u+124u,0x4858u);
    CHECK(core->probe(&host,&variant)==IFS_FS_UNSUPPORTED);
    be16(1024u+124u,0);
    be16(1024u+34u,101u);
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT);
    be16(1024u+34u,40u);
    be32(1024u+20u,UINT32_MAX);
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT);
    be32(1024u+20u,4096u);
    fail_reads=1;
    CHECK(core->probe(&host,&variant)==IFS_FS_IO_ERROR);
    fail_reads=0;
    host.read_at=NULL;
    CHECK(core->probe(&host,&variant)==IFS_FS_INVALID);
    puts("HFS-STRICT-COMMON-V2-MOUNT-QUERY-PASS");
    return 0;
}

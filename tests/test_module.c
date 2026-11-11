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
static int rd(void *ctx,uint64_t at,void *dst,uint32_t n) {
    (void)ctx;
    if(fail_reads || at>SIZE || n>SIZE-at)return -1;
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
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"HFS V2 failure: %s line %d\n",#x,__LINE__);return 1;}}while(0)
int main(void) {
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"hfs"));
    CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    CHECK(api->group_mask==(IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                            IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)));
    const IfsFsCoreOpsV2 *core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *ops=(const IfsFsVolumeOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(core && core->header.capabilities==(IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT));
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
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1u);
    IfsFsVolumeInfoV2 info={0},recheck={0};
    void *mounted=NULL,*stale=NULL;
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_OK && mounted);
    stale=mounted;
    CHECK(info.variant==1u && info.block_bytes==4096u &&
          info.total_bytes==409600u && info.free_bytes==163840u &&
          info.max_name_bytes==31u);
    recheck.structure_bytes=sizeof(recheck);
    CHECK(ops->query(mounted,&recheck)==IFS_FS_OK &&
          recheck.free_bytes==info.free_bytes &&
          recheck.total_bytes==info.total_bytes);
    {
        void *duplicate=NULL;
        CHECK(core->mount(&host,&duplicate,&recheck)==IFS_FS_BUSY);
        CHECK(duplicate==NULL);
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

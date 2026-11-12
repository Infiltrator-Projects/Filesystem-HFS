/* SPDX-License-Identifier: GPL-3.0-or-later
 * Classic HFS provider: strict Common V2 read-only probe, mount and volume
 * reporting over the validated on-media Master Directory Block (MDB).
 * File writes, recovery and relocation are deliberately NOT advertised.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stddef.h>

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0]<<8)|(uint16_t)p[1]);
}
static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
/* Classic HFS MDB is at partition-relative offset 1024. A wrapped HFS+
 * image has the classic BD signature but is not a classic HFS volume. */
static int examine(const IfsFsHostRuntimeV2 *host,uint32_t *variant,
                   IfsFsVolumeInfoV2 *details)
{
    unsigned char mdb[512];
    uint64_t bitmap_start,allocation,physical,bitmap_end;
    uint32_t block_bytes;
    uint16_t blocks,bitmap,first,free_blocks;
    if(!variant || !ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    *variant=0u;
    if(host->volume_bytes<1536u)return IFS_FS_NOT_FOUND;
    {
        /* The MDB is at byte 1024, not necessarily a sector boundary.
         * Common V2 makes the authenticated physical reads sector-aligned. */
        int rc=ifs_fs_host_read_bytes_v2(host,1024u,mdb,(uint32_t)sizeof(mdb));
        if(rc!=IFS_FS_OK)return rc;
    }
    if(be16(mdb)!=UINT16_C(0x4244))return IFS_FS_NOT_FOUND;
    if(be16(mdb+124u)==UINT16_C(0x482b) ||
       be16(mdb+124u)==UINT16_C(0x4858))
        return IFS_FS_UNSUPPORTED; /* HFS+ or HFSX wrapper */
    bitmap=be16(mdb+14u);
    blocks=be16(mdb+18u);
    block_bytes=be32(mdb+20u);
    first=be16(mdb+28u);
    free_blocks=be16(mdb+34u);
    if(!bitmap || !blocks || free_blocks>blocks || !first ||
       block_bytes<512u || block_bytes%512u)
        return IFS_FS_CORRUPT;
    bitmap_start=(uint64_t)bitmap*512u;
    allocation=(uint64_t)first*512u;
    physical=(uint64_t)blocks*block_bytes;
    if(bitmap_start<1536u || bitmap_start>=allocation ||
       allocation>host->volume_bytes ||
       physical>host->volume_bytes-allocation)
        return IFS_FS_CORRUPT;
    bitmap_end=bitmap_start+((uint64_t)blocks+7u)/8u;
    if(bitmap_end>allocation)
        return IFS_FS_CORRUPT;
    /* The MDB free count is only a recorded summary. Verify it against
     * the real big-endian allocation bitmap, whose MSB-first bit 1 means
     * allocated (as in the repository's native HFS bitmap engine).
     * Reject a stale or damaged count before publishing VOLUME.QUERY. */
    const uint32_t map_bytes=((uint32_t)blocks+7u)/8u;
    unsigned char *map=host->allocate(host->context,map_bytes);
    if(!map)return IFS_FS_NO_MEMORY;
    int read_rc=ifs_fs_host_read_bytes_v2(host,bitmap_start,map,map_bytes);
    if(read_rc!=IFS_FS_OK) {
        host->release(host->context,map);
        return read_rc;
    }
    uint32_t verified_free=0u;
    for(uint32_t index=0u;index<blocks;++index)
        if(!(map[index>>3u]&(unsigned char)(0x80u>>(index&7u))))
            ++verified_free;
    host->release(host->context,map);
    if(verified_free!=free_blocks)
        return IFS_FS_CORRUPT;
    *variant=1u;
    if(details) {
        details->structure_bytes=sizeof(*details);
        details->variant=1u;
        details->max_name_bytes=31u; /* HFS Pascal filename (MacRoman bytes) */
        details->block_bytes=block_bytes;
        details->total_bytes=physical;
        details->free_bytes=(uint64_t)free_blocks*block_bytes;
    }
    return IFS_FS_OK;
}
static int probe(const IfsFsHostRuntimeV2 *host,uint32_t *variant)
{
    return examine(host,variant,NULL);
}
/* A module image owns exactly one active read-only session, with a generation
 * token rather than a reusable pointer. No media is written or allocated. */
static struct {
    uintptr_t generation,active;
    IfsFsVolumeInfoV2 details;
} session;
static int mounted(void *volume)
{
    return session.active!=0u && (uintptr_t)volume==session.active;
}
static int mount_volume(const IfsFsHostRuntimeV2 *host,void **volume,
                        IfsFsVolumeInfoV2 *info)
{
    IfsFsVolumeInfoV2 observed={0};
    uint32_t variant=0u;
    int rc;
    if(!volume || !info || info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    *volume=NULL;
    if(!ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    /* CORE.MOUNT is read-only. A valid exclusive writable host is NOT
     * interchangeable with a read-only host, even when no write callback
     * is advertised by this provider. */
    if(!(host->capabilities & IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    if(session.active || session.generation==UINTPTR_MAX)
        return IFS_FS_BUSY;
    rc=examine(host,&variant,&observed);
    if(rc!=IFS_FS_OK)return rc;
    session.active=++session.generation;
    session.details=observed;
    *info=observed;
    *volume=(void *)session.active;
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    if(!mounted(volume))return IFS_FS_INVALID;
    session.active=0u;
    return IFS_FS_OK;
}
static int query_volume(void *volume,IfsFsVolumeInfoV2 *info)
{
    if(!mounted(volume) || !info ||
       info->structure_bytes<sizeof(*info))return IFS_FS_INVALID;
    *info=session.details;
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,
             IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT},
    .probe=probe,.mount=mount_volume,.unmount=unmount_volume
};
static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY},
    .query=query_volume
};
static const IfsFsGroupHeaderV2 *query(
    uint32_t id,uint32_t minimum,uint32_t maximum)
{
    if(minimum>IFS_FS_GROUP_ABI_V2 || maximum<IFS_FS_GROUP_ABI_V2)
        return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume_ops.header;
    return NULL;
}
__attribute__((used,section(".ifsmeta")))
static const char identity[]="IFS-MODULE:hfs:common2";
static const IfsFsModuleApiV2 api={
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="hfs",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) {return &api;}

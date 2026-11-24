/* SPDX-License-Identifier: GPL-3.0-or-later
 * Classic HFS Common V2 native reader: validated MDB/bitmap, genuine catalog
 * B-tree lookup, directory enumeration and checked read-only data forks.
 * Hosted and freestanding profiles share the same format-owned implementation.
 * Writes, recovery and relocation are not advertised.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "hfs_catalog_v2.h"
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

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
    if(!variant)return IFS_FS_INVALID;
    *variant=0u;
    if(!ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
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
static int probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    return examine(host, variant, NULL);
}
/* Stable, non-recycled mount handles and independent per-volume B-tree
 * state. No pointer to caller stack data survives a successful mount. */
#define HFS_MAX_MOUNTS 32u
typedef struct {
    uintptr_t token;
    unsigned busy;
    IfsFsHostRuntimeV2 host;
    IfsFsVolumeInfoV2 info;
    HfsCatalogueV2 catalog;
} HfsMount;
static HfsMount mounts[HFS_MAX_MOUNTS];
static uintptr_t next_token;
/* A short registry lock protects token allocation and exclusive per-volume
 * operation leases. The lock is never held across host I/O. A busy volume
 * returns BUSY rather than racing catalogue state or blocking a callback. */
static atomic_flag registry_guard=ATOMIC_FLAG_INIT;
static void lock_registry(void)
{
    while (atomic_flag_test_and_set_explicit(&registry_guard,memory_order_acquire)) {}
}
static void unlock_registry(void)
{
    atomic_flag_clear_explicit(&registry_guard,memory_order_release);
}
static int acquire_mount(void *handle,HfsMount **out)
{
    int rc=IFS_FS_INVALID;
    *out=NULL;
    if (!handle) return rc;
    lock_registry();
    for (unsigned i=0;i<HFS_MAX_MOUNTS;++i) {
        if (mounts[i].token!=(uintptr_t)handle) continue;
        if (mounts[i].busy) rc=IFS_FS_BUSY;
        else {
            mounts[i].busy=1u;
            *out=&mounts[i];
            rc=IFS_FS_OK;
        }
        break;
    }
    unlock_registry();
    return rc;
}
static int release_mount(HfsMount *slot,int rc)
{
    lock_registry();
    slot->busy=0u;
    unlock_registry();
    return rc;
}
static int mount_volume(const IfsFsHostRuntimeV2 *host, void **volume,
                        IfsFsVolumeInfoV2 *info)
{
    IfsFsVolumeInfoV2 measured={0};
    HfsMount *slot=NULL;
    uint32_t variant=0u;
    if (!volume || !info || info->structure_bytes<sizeof(*info) ||
        !ifs_fs_host_v2_valid(host)) return IFS_FS_INVALID;
    *volume=NULL;
    if (!(host->capabilities&IFS_FS_HOST_READ_ONLY)) return IFS_FS_ACCESS;
    int rc=examine(host,&variant,&measured);
    if (rc!=IFS_FS_OK) return rc;
    lock_registry();
    for (unsigned i=0;i<HFS_MAX_MOUNTS;++i)
        if (!mounts[i].token && !mounts[i].busy) {
            slot=&mounts[i];
            break;
        }
    if (!slot || next_token==UINTPTR_MAX) {
        unlock_registry();
        return IFS_FS_BUSY;
    }
    /* Reserve the stable slot before opening the catalogue, but do not
     * publish its token until the native reader has fully initialized. */
    slot->busy=1u;
    const uintptr_t token=++next_token;
    unlock_registry();
    /* The Common V2 host structure is size-versioned. Copy only the
     * caller-supplied prefix; never read unowned extension bytes. */
    memset(&slot->host,0,sizeof(slot->host));
    size_t host_bytes=host->structure_bytes<sizeof(slot->host)?
        host->structure_bytes:sizeof(slot->host);
    memcpy(&slot->host,host,host_bytes);
    slot->host.structure_bytes=(uint32_t)host_bytes;
    slot->info=measured;
    rc=hfs_catalogue_open_v2(&slot->host,&slot->catalog);
    if (rc!=IFS_FS_OK) {
        slot->host=(IfsFsHostRuntimeV2){0};
        slot->info=(IfsFsVolumeInfoV2){0};
        return release_mount(slot,rc);
    }
    lock_registry();
    slot->token=token;
    slot->busy=0u;
    *info=measured;
    *volume=(void *)token;
    unlock_registry();
    return IFS_FS_OK;
}
static int unmount_volume(void *volume)
{
    HfsMount *slot;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    hfs_catalogue_close_v2(&slot->catalog);
    slot->host=(IfsFsHostRuntimeV2){0};
    slot->info=(IfsFsVolumeInfoV2){0};
    lock_registry();
    slot->token=0u;
    slot->busy=0u;
    unlock_registry();
    return IFS_FS_OK;
}
static int query_volume(void *volume,IfsFsVolumeInfoV2 *info)
{
    HfsMount *slot;
    if (!info || info->structure_bytes<sizeof(*info)) return IFS_FS_INVALID;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    *info=slot->info;
    return release_mount(slot,IFS_FS_OK);
}
static int stat_file(void *volume,const char *path,IfsFsEntryV2 *entry)
{
    HfsMount *slot;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    return release_mount(slot,hfs_catalogue_stat_v2(&slot->catalog,path,entry));
}
static int list_file(void *volume,const char *path,uint32_t index,
                     IfsFsEntryV2 *entry)
{
    HfsMount *slot;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    return release_mount(slot,hfs_catalogue_list_v2(&slot->catalog,path,index,entry));
}
static int read_file(void *volume,const char *path,uint64_t offset,
                     void *destination,uint32_t length)
{
    HfsMount *slot;
    int rc=acquire_mount(volume,&slot);
    if (rc!=IFS_FS_OK) return rc;
    return release_mount(slot,hfs_catalogue_read_v2(&slot->catalog,path,offset,destination,length));
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,
             IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|
             IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
    .probe=probe,.mount=mount_volume,.unmount=unmount_volume,
    .stat=stat_file,.directory_entry=list_file,.read=read_file
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
    #ifdef IFS_HFS_FIRMWARE_BUILD
    .runtime_profiles=IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
#else
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
#endif
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) {return &api;}

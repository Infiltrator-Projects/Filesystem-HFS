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
#ifndef IFS_HFS_FIRMWARE_BUILD
static unsigned formatting;
#endif
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
#ifndef IFS_HFS_FIRMWARE_BUILD
    if(formatting){unlock_registry();return IFS_FS_BUSY;}
#endif
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
#ifndef IFS_HFS_FIRMWARE_BUILD
/* Classic HFS empty-volume creation. Use 512-byte logical B-tree nodes
 * and a separately sized allocation heap. All media transfers go through
 * the Common V2 lease, including 4Kn read-modify-write of the MDB.
 * The bounded first profile uses printable MacRoman (ASCII subset). */
enum { HFS_FMT_NODE=512u, HFS_FMT_HEAP=16384u, HFS_FMT_BITMAP=8192u };
static void store16(uint8_t *p,uint32_t x)
{p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void store32(uint8_t *p,uint32_t x)
{store16(p,x>>16);store16(p+2,x);}
static int format_write(const IfsFsHostRuntimeV2 *h,uint64_t at,const void *p,uint32_t n)
{
    if(h->cancelled && h->cancelled(h->context))return IFS_FS_BUSY;
    return ifs_fs_host_write_bytes_v2(h,at,p,n);
}
static void format_tree(uint8_t *p,uint32_t nodes,int catalog)
{
    memset(p,0,HFS_FMT_NODE);
    p[8]=1u;store16(p+10,3u);
    if(catalog) {
        store16(p+14,1u);store32(p+16,1u);store32(p+20,2u);
        store32(p+24,1u);store32(p+28,1u);
    }
    store16(p+32,HFS_FMT_NODE);store16(p+34,catalog?37u:7u);
    store32(p+36,nodes);store32(p+40,nodes-(catalog?2u:1u));
    p[248]=catalog?0xc0u:0x80u;
    store16(p+510,14u);store16(p+508,120u);
    store16(p+506,248u);store16(p+504,504u);
}
static uint32_t format_key(uint8_t *p,uint32_t parent,const char *name,uint32_t n)
{
    /* The one-byte key length excludes itself, but each record's data
     * begins on an even boundary. The reserved byte stays zero. */
    p[0]=(uint8_t)(6u+n);store32(p+2,parent);p[6]=(uint8_t)n;
    memcpy(p+7,name,n);
    return (8u+n)&~1u;
}
static int format_impl(const IfsFsHostRuntimeV2 *h,const char *label)
{
    const uint64_t available=h->volume_bytes-HFS_FMT_HEAP-1024u;
    uint32_t unit=4096u;
    while(available/unit>UINT16_MAX)unit*=2u;
    const uint32_t blocks=(uint32_t)(available/unit);
    uint32_t n=(uint32_t)strlen(label),now=2082844800u;
    if(h->realtime_ns) {
        uint64_t t=h->realtime_ns(h->context)/1000000000ull;
        if(t<=UINT32_MAX-2082844800u)now+=(uint32_t)t;
    }
    uint8_t *tree=h->allocate(h->context,unit);
    if(!tree)return IFS_FS_NO_MEMORY;
    uint8_t bitmap[HFS_FMT_BITMAP]={0},mdb[512]={0};
    int rc;
    /* Invalidate primary and alternate MDB before writing allocation
     * records. Zero every reserved sector ahead of the allocation heap. */
    for(uint32_t at=0u;at<HFS_FMT_HEAP;at+=sizeof(bitmap)) {
        rc=format_write(h,at,bitmap,sizeof(bitmap));
        if(rc!=IFS_FS_OK)goto done;
    }
    rc=format_write(h,h->volume_bytes-1024u,bitmap,1024u);
    if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}

    /* Allocation blocks zero and one own the extent and catalog trees.
     * Reserve nonexistent bitmap positions so they cannot be allocated. */
    bitmap[0]=0xc0u;
    for(uint32_t bit=blocks;bit<HFS_FMT_BITMAP*8u;++bit)
        bitmap[bit/8u]|=(uint8_t)(0x80u>>(bit%8u));
    rc=format_write(h,1536u,bitmap,sizeof(bitmap));
    if(rc!=IFS_FS_OK)goto done;
    memset(tree,0,unit);
    format_tree(tree,unit/HFS_FMT_NODE,0);
    rc=format_write(h,HFS_FMT_HEAP,tree,unit);
    if(rc!=IFS_FS_OK)goto done;
    memset(tree,0,unit);
    format_tree(tree,unit/HFS_FMT_NODE,1);
    /* Two ordered catalogue records: root directory and its mandatory
     * thread record. The classic folder structure is 70 bytes; the
     * thread stores the parent's CNID followed by a Str31 name. */
    uint8_t *leaf=tree+HFS_FMT_NODE;
    leaf[8]=0xffu;leaf[9]=1u;store16(leaf+10,2u);
    uint32_t at=14u;store16(leaf+510,at);
    uint8_t *value=leaf+at+format_key(leaf+at,1u,label,n);
    value[0]=1u;store32(value+6,2u);
    store32(value+10,now);store32(value+14,now);
    at=(uint32_t)(value-leaf)+70u;store16(leaf+508,at);
    value=leaf+at+format_key(leaf+at,2u,"",0u);
    value[0]=3u;store32(value+10,1u);
    value[14]=(uint8_t)n;memcpy(value+15,label,n);
    at=(uint32_t)(value-leaf)+46u;store16(leaf+506,at);
    rc=format_write(h,HFS_FMT_HEAP+unit,tree,unit);
    if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}

    store16(mdb,0x4244u);store32(mdb+2,now);store32(mdb+6,now);
    store16(mdb+10,0x100u);store16(mdb+14,3u);
    store16(mdb+16,2u);store16(mdb+18,blocks);
    store32(mdb+20,unit);store32(mdb+24,unit);
    store16(mdb+28,HFS_FMT_HEAP/512u);store32(mdb+30,16u);
    store16(mdb+34,blocks-2u);mdb[36]=(uint8_t)n;memcpy(mdb+37,label,n);
    store32(mdb+70,1u);store32(mdb+74,unit);store32(mdb+78,unit);
    store32(mdb+130,unit);store16(mdb+136,1u);
    store32(mdb+146,unit);store16(mdb+150,1u);store16(mdb+152,1u);
    rc=format_write(h,h->volume_bytes-1024u,mdb,sizeof(mdb));
    if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0){rc=IFS_FS_IO_ERROR;goto done;}
    rc=format_write(h,1024u,mdb,sizeof(mdb));
    if(rc!=IFS_FS_OK)goto done;
    if(h->flush(h->context)!=0) {
        memset(mdb,0,sizeof(mdb));
        (void)ifs_fs_host_write_bytes_v2(h,1024u,mdb,sizeof(mdb));
        (void)ifs_fs_host_write_bytes_v2(h,h->volume_bytes-1024u,mdb,sizeof(mdb));
        (void)h->flush(h->context);rc=IFS_FS_IO_ERROR;goto done;
    }
    uint32_t observed=0u;
    rc=probe(h,&observed);
    if(rc!=IFS_FS_OK)goto done;
    if(observed!=1u){rc=IFS_FS_CORRUPT;goto done;}
    HfsCatalogueV2 check;
    rc=hfs_catalogue_open_v2(h,&check);
    if(rc==IFS_FS_OK)hfs_catalogue_close_v2(&check);
done:
    h->release(h->context,tree);
    return rc;
}
static int format_volume(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                          const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    if(variant!=1u || flags || h->logical_sector_bytes>4096u ||
       h->volume_bytes%4096u || h->volume_bytes<32ull*1024u*1024u ||
       h->volume_bytes>2ull*1024u*1024u*1024u)return IFS_FS_UNSUPPORTED;
    if(!label || !*label)label="Untitled";
    const size_t n=strlen(label);
    if(n>27u)return IFS_FS_INVALID;
    for(size_t i=0u;i<n;++i)
        if((unsigned char)label[i]<32u || (unsigned char)label[i]>126u ||
           label[i]==':' || label[i]=='/')return IFS_FS_INVALID;
    lock_registry();
    if(formatting){unlock_registry();return IFS_FS_BUSY;}
    for(unsigned i=0u;i<HFS_MAX_MOUNTS;++i)
        if(mounts[i].token || mounts[i].busy) {
            unlock_registry();return IFS_FS_BUSY;
        }
    formatting=1u;unlock_registry();
    const int rc=format_impl(h,label);
    lock_registry();formatting=0u;unlock_registry();
    return rc;
}
#endif

static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY
#ifndef IFS_HFS_FIRMWARE_BUILD
             |IFS_FS_VOLUME_FORMAT
#endif
    },
    .query=query_volume
#ifndef IFS_HFS_FIRMWARE_BUILD
    ,.format=format_volume
#endif
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

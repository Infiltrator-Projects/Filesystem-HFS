/* SPDX-License-Identifier: GPL-3.0-or-later
 * Classic HFS Common V2 native reader: validated MDB/bitmap, genuine catalog
 * B-tree lookup, directory enumeration and checked read-only data forks.
 * Hosted and freestanding profiles share the same format-owned implementation.
 * Hosted creation and append are bounded to the native formatter's flat-root
 * profile. Firmware, general mutation, journal recovery and relocation remain read-only or unsupported.
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
    unsigned busy, writable, mutation_fault;
    IfsFsHostRuntimeV2 host;
    IfsFsVolumeInfoV2 info;
    HfsCatalogueV2 catalog;
} HfsMount;
static HfsMount mounts[HFS_MAX_MOUNTS];
static uintptr_t next_token;
#ifndef IFS_HFS_FIRMWARE_BUILD
static unsigned formatting;
static int writable_profile(HfsMount *slot);
static int create_file(void *,const char *,uint32_t);
static int write_file(void *,const char *,uint64_t,const void *,uint32_t);
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
    if(!slot->token)slot->writable=0u;
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
    const unsigned writable=(host->capabilities&IFS_FS_HOST_READ_ONLY)==0u;
#ifdef IFS_HFS_FIRMWARE_BUILD
    if(writable)return IFS_FS_ACCESS;
#else
    if(writable && (!(host->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(host->capabilities&IFS_FS_HOST_DURABLE)))return IFS_FS_ACCESS;
#endif
    lock_registry();
#ifndef IFS_HFS_FIRMWARE_BUILD
    if(formatting){unlock_registry();return IFS_FS_BUSY;}
#endif
    /* No stable media identity exists in this ABI: serialize writable leases
     * against all live mounts, including a mount whose callbacks are active. */
    for(unsigned i=0;i<HFS_MAX_MOUNTS;++i)
        if((mounts[i].token || mounts[i].busy) && (writable || mounts[i].writable)) {
            unlock_registry();return IFS_FS_BUSY;
        }
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
    slot->writable=writable;
    slot->mutation_fault=0u;
    const uintptr_t token=++next_token;
    unlock_registry();
    /* Reserve the slot before any media read: a reentrant formatter must
     * not replace the MDB between geometry inspection and catalogue open. */
    int rc=examine(host,&variant,&measured);
    if (rc!=IFS_FS_OK) return release_mount(slot,rc);
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
    #ifndef IFS_HFS_FIRMWARE_BUILD
    if(writable && writable_profile(slot)!=IFS_FS_OK) {
        hfs_catalogue_close_v2(&slot->catalog);
        slot->host=(IfsFsHostRuntimeV2){0};
        slot->info=(IfsFsVolumeInfoV2){0};
        return release_mount(slot,IFS_FS_ACCESS);
    }
#endif
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
    slot->writable=0u;
    slot->mutation_fault=0u;
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
static int map_file(void *volume,const char *path,uint64_t first,
                    IfsFsExtentV2 *out,uint32_t capacity,uint32_t *count)
{
    if(!count)return IFS_FS_INVALID;
    *count=0u;
    HfsMount *slot;
    int rc=acquire_mount(volume,&slot);
    if(rc!=IFS_FS_OK)return rc;
    return release_mount(slot,hfs_catalogue_map_v2(&slot->catalog,path,
                         first,out,capacity,count));
}
static const IfsFsDefragOpsV2 defrag_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsDefragOpsV2),
             IFS_FS_GROUP_DEFRAG,0u,IFS_FS_DEFRAG_MAP},
    .map=map_file
};
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,
             IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|
             IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ
#ifndef IFS_HFS_FIRMWARE_BUILD
             |IFS_FS_CORE_CREATE|IFS_FS_CORE_WRITE
#endif
    },
    .probe=probe,.mount=mount_volume,.unmount=unmount_volume,
    .stat=stat_file,.directory_entry=list_file,.read=read_file
#ifndef IFS_HFS_FIRMWARE_BUILD
    ,.create=create_file,.write=write_file
#endif
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
/* Native bounded HFS mutation. All catalogue keys and allocation extents
 * are real on-disk HFS records. This is not a fixture-specific API: callers
 * create arbitrary supported flat-root names and append opaque bytes.
 *
 * Only the formatter's single-allocation-block catalogue, inline extents,
 * empty extent-overflow tree and root directory are writable. Reject other
 * layouts rather than attempt unqualified B-tree growth or repair.
 */
enum { HFS_MUT_FILES=24u, HFS_MUT_RECORDS=HFS_MUT_FILES+2u };
typedef struct { uint8_t data[144]; uint32_t length; } HfsRecord;
typedef struct {
    HfsMount *slot;
    uint8_t mdb[512],oldmdb[512];
    uint8_t bitmap[HFS_FMT_BITMAP],oldbitmap[HFS_FMT_BITMAP];
    uint8_t *tree,*oldtree;
    uint32_t unit,blocks,count,bitmap_bytes;
    HfsRecord records[HFS_MUT_RECORDS];
} HfsMutation;

static uint32_t record_key_bytes(const HfsRecord *r)
{return ((uint32_t)r->data[0]+2u)&~1u;}
static uint8_t *record_value(HfsRecord *r)
{return r->data+record_key_bytes(r);}
static int used_block(const uint8_t *map,uint32_t b)
{return (map[b/8u] & (uint8_t)(0x80u>>(b%8u)))!=0u;}
static int name_supported(const char *name,uint32_t n)
{
    if(!n || n>31u)return 0;
    for(uint32_t i=0;i<n;++i)
        if(!((name[i]>='A' && name[i]<='Z') ||
             (name[i]>='0' && name[i]<='9') || name[i]=='.'))
            return 0;
    return !(n==1u && name[0]=='.') &&
           !(n==2u && name[0]=='.' && name[1]=='.');
}
static int compare_name(const HfsRecord *r,const char *name,uint32_t n)
{
    const uint32_t rn=r->data[6], common=rn<n?rn:n;
    int c=memcmp(r->data+7u,name,common);
    return c?c:rn<n?-1:rn>n?1:0;
}
static int append_record(uint8_t *node,uint32_t *offset,uint32_t ordinal,
                         const uint8_t *record,uint32_t bytes)
{
    if(*offset>512u-2u*(ordinal+2u) ||
       bytes>512u-2u*(ordinal+2u)-*offset)return IFS_FS_NO_SPACE;
    store16(node+510u-2u*ordinal,*offset);
    memcpy(node+*offset,record,bytes);
    *offset+=bytes;
    store16(node+508u-2u*ordinal,*offset);
    return IFS_FS_OK;
}
/* Repack the small catalogue into real ordered leaf records and, when
 * necessary, a depth-two index. A classic index key has the fixed maximum
 * key length; its actual Pascal name length remains in the key itself. */
static int rebuild_catalogue(HfsMutation *m)
{
    uint32_t starts[HFS_MUT_RECORDS],counts[HFS_MUT_RECORDS],leaves=0u;
    uint32_t i=0u;
    while(i<m->count) {
        uint32_t at=14u,n=0u;
        starts[leaves]=i;
        while(i<m->count && at+m->records[i].length<=512u-2u*(n+2u)) {
            at+=m->records[i++].length;++n;
        }
        if(!n)return IFS_FS_NO_SPACE;
        counts[leaves++]=n;
    }
    const uint32_t first=leaves==1u?1u:2u,used=first+leaves;
    const uint32_t nodes=m->unit/512u;
    if(used>nodes)return IFS_FS_NO_SPACE;
    memset(m->tree,0,m->unit);
    format_tree(m->tree,nodes,1);
    store16(m->tree+14u,leaves==1u?1u:2u);
    store32(m->tree+16u,1u);
    store32(m->tree+20u,m->count);
    store32(m->tree+24u,first);
    store32(m->tree+28u,used-1u);
    store32(m->tree+40u,nodes-used);
    memset(m->tree+248u,0,256u);
    for(uint32_t b=0u;b<used;++b)
        m->tree[248u+b/8u]|=(uint8_t)(0x80u>>(b%8u));
    uint32_t index_at=14u;
    uint8_t *index=m->tree+512u;
    if(leaves>1u) {
        index[8]=0u;index[9]=2u;store16(index+10u,leaves);
    }
    for(uint32_t l=0u;l<leaves;++l) {
        const uint32_t number=first+l;
        uint8_t *node=m->tree+number*512u;
        store32(node,l+1u<leaves?number+1u:0u);
        /* The previous leaf belongs to bLink (offset 4), not fLink. */
        store32(node+4u,l?number-1u:0u);
        node[8]=0xffu;node[9]=1u;
        store16(node+10u,counts[l]);
        uint32_t at=14u;
        for(uint32_t j=0u;j<counts[l];++j) {
            HfsRecord *r=&m->records[starts[l]+j];
            int rc=append_record(node,&at,j,r->data,r->length);
            if(rc!=IFS_FS_OK)return rc;
        }
        if(leaves>1u) {
            uint8_t key[42]={0};
            HfsRecord *r=&m->records[starts[l]];
            memcpy(key,r->data,record_key_bytes(r));
            key[0]=37u;store32(key+38u,number);
            int rc=append_record(index,&index_at,l,key,sizeof(key));
            if(rc!=IFS_FS_OK)return rc;
        }
    }
    return IFS_FS_OK;
}
static void mutation_free(HfsMutation *m)
{
    const IfsFsHostRuntimeV2 *h=&m->slot->host;
    if(m->oldtree)h->release(h->context,m->oldtree);
    if(m->tree)h->release(h->context,m->tree);
}
static int mutation_load(HfsMutation *m,HfsMount *slot)
{
    m->slot=slot;
    const IfsFsHostRuntimeV2 *h=&slot->host;
    const HfsCatalogueV2 *v=&slot->catalog;
    if(slot->mutation_fault)return IFS_FS_IO_ERROR;
    if(!slot->writable)return IFS_FS_ACCESS;
    m->unit=v->allocation_bytes;m->blocks=v->allocation_blocks;
    m->bitmap_bytes=v->bitmap_bytes;
    if(v->allocation_start!=HFS_FMT_HEAP || m->unit<4096u ||
       m->unit>32768u || (m->unit&(m->unit-1u)) ||
       v->catalogue_size!=m->unit || v->node_bytes!=512u ||
       v->catalogue_extents[0].block_start!=1u ||
       v->catalogue_extents[0].block_count!=1u ||
       v->catalogue_extents[1].block_count || v->catalogue_extents[2].block_count ||
       m->bitmap_bytes>HFS_FMT_BITMAP)return IFS_FS_UNSUPPORTED;
    int rc=ifs_fs_host_read_bytes_v2(h,1024u,m->oldmdb,sizeof(m->oldmdb));
    if(rc!=IFS_FS_OK)return rc;
    rc=ifs_fs_host_read_bytes_v2(h,h->volume_bytes-1024u,m->mdb,sizeof(m->mdb));
    if(rc!=IFS_FS_OK)return rc;
    if(memcmp(m->mdb,m->oldmdb,sizeof(m->mdb)) ||
       be16(m->mdb+10u)!=0x100u || be16(m->mdb+14u)!=3u ||
       be32(m->mdb+130u)!=m->unit || be16(m->mdb+134u)!=0u ||
       be16(m->mdb+136u)!=1u || be16(m->mdb+138u) || be16(m->mdb+140u) ||
       be16(m->mdb+142u) || be16(m->mdb+144u))
        return IFS_FS_UNSUPPORTED;
    m->oldtree=h->allocate(h->context,m->unit);
    m->tree=h->allocate(h->context,m->unit);
    if(!m->oldtree || !m->tree)return IFS_FS_NO_MEMORY;
    rc=ifs_fs_host_read_bytes_v2(h,HFS_FMT_HEAP+m->unit,m->oldtree,m->unit);
    if(rc!=IFS_FS_OK)return rc;
    rc=ifs_fs_host_read_bytes_v2(h,HFS_FMT_HEAP,m->tree,m->unit);
    if(rc!=IFS_FS_OK)return rc;
    /* The formatter's empty overflow tree must remain byte-identical. */
    uint8_t empty[512];format_tree(empty,m->unit/512u,0);
    if(memcmp(empty,m->tree,512u))return IFS_FS_UNSUPPORTED;
    for(uint32_t j=512u;j<m->unit;++j)
        if(m->tree[j])return IFS_FS_UNSUPPORTED;
    rc=ifs_fs_host_read_bytes_v2(h,1536u,m->oldbitmap,HFS_FMT_BITMAP);
    if(rc!=IFS_FS_OK)return rc;
    memcpy(m->bitmap,m->oldbitmap,sizeof(m->bitmap));
    uint8_t expected[HFS_FMT_BITMAP]={0};
    expected[0]=0xc0u;
    const uint32_t nodes=m->unit/512u;
    uint32_t next=v->first_leaf,previous=0u,files=0u,free_count=0u;
    if(!next)return IFS_FS_CORRUPT;
    for(uint32_t visited=0u;next;++visited) {
        if(visited>=nodes || next>=nodes)return IFS_FS_CORRUPT;
        const uint8_t *node=m->oldtree+next*512u;
        uint32_t n=be16(node+10u);
        if(!n || n>248u || node[8]!=0xffu || node[9]!=1u ||
           be32(node+4u)!=previous)return IFS_FS_CORRUPT;
        for(uint32_t j=0u;j<n;++j) {
            uint32_t from=be16(node+510u-2u*j),to=be16(node+508u-2u*j);
            if(m->count>=HFS_MUT_RECORDS || from<14u || to<=from ||
               to>512u-2u*(n+1u) || to-from>sizeof(m->records[0].data) ||
               to-from<8u)return IFS_FS_UNSUPPORTED;
            HfsRecord *r=&m->records[m->count];
            r->length=to-from;memcpy(r->data,node+from,r->length);
            uint32_t key=record_key_bytes(r),parent=be32(r->data+2u),nl=r->data[6];
            if(nl>31u || r->data[0]!=6u+nl || key>=r->length)
                return IFS_FS_CORRUPT;
            uint8_t *d=record_value(r);
            if(m->count==0u) {
                if(parent!=1u || d[0]!=1u || r->length-key!=70u ||
                   be32(d+6u)!=2u || nl!=m->mdb[36] ||
                   memcmp(r->data+7u,m->mdb+37u,nl))return IFS_FS_UNSUPPORTED;
            } else if(m->count==1u) {
                if(parent!=2u || nl || d[0]!=3u || r->length-key!=46u ||
                   be32(d+10u)!=1u || d[14]!=m->mdb[36] ||
                   memcmp(d+15u,m->mdb+37u,d[14]))return IFS_FS_UNSUPPORTED;
            } else {
                if(parent!=2u || d[0]!=2u || r->length-key!=102u ||
                   !name_supported((const char *)r->data+7u,nl) ||
                   be32(d+20u)<16u || be32(d+20u)>=be32(m->mdb+30u) ||
                   be16(d+34u) || be32(d+36u) || be32(d+40u))
                    return IFS_FS_UNSUPPORTED;
                if(m->count>2u && compare_name(&m->records[m->count-1u],
                   (const char *)r->data+7u,nl)>=0)return IFS_FS_CORRUPT;
                for(uint32_t k=2u;k<m->count;++k)
                    if(be32(record_value(&m->records[k])+20u)==be32(d+20u))
                        return IFS_FS_CORRUPT;
                uint32_t total=0u;int ended=0;
                for(uint32_t k=0u;k<3u;++k) {
                    uint32_t b=be16(d+74u+4u*k),len=be16(d+76u+4u*k);
                    if(!len){if(b)return IFS_FS_CORRUPT;ended=1;continue;}
                    if(ended || b<2u || b>=m->blocks || len>m->blocks-b)
                        return IFS_FS_CORRUPT;
                    for(uint32_t x=b;x<b+len;++x) {
                        if(used_block(expected,x))return IFS_FS_CORRUPT;
                        expected[x/8u]|=(uint8_t)(0x80u>>(x%8u));
                    }
                    total+=len;
                }
                if((uint64_t)total*m->unit!=be32(d+30u) ||
                   ((uint64_t)be32(d+26u)+m->unit-1u)/m->unit!=total ||
                   be16(d+24u)!=be16(d+74u))return IFS_FS_CORRUPT;
                ++files;
            }
            ++m->count;
        }
        previous=next;next=be32(node);
    }
    if(m->count<2u || files!=be16(m->mdb+12u) ||
       files!=be32(m->mdb+84u) ||
       files!=be16(record_value(&m->records[0])+4u))return IFS_FS_CORRUPT;
    for(uint32_t b=0u;b<m->blocks;++b) {
        int present=used_block(m->bitmap,b);
        if(present!=used_block(expected,b))return IFS_FS_UNSUPPORTED;
        if(!present)++free_count;
    }
    if(free_count!=be16(m->mdb+34u))return IFS_FS_CORRUPT;
    /* Refuse foreign or inconsistent index structures before any mutation.
     * Rebuilding the exact supported catalogue is also a structural oracle. */
    rc=rebuild_catalogue(m);
    if(rc!=IFS_FS_OK)return rc;
    return memcmp(m->tree,m->oldtree,m->unit)?IFS_FS_UNSUPPORTED:IFS_FS_OK;
}
static int writable_profile(HfsMount *slot)
{
    HfsMutation m={0};
    int rc=mutation_load(&m,slot);
    mutation_free(&m);return rc;
}
static uint32_t mutation_time(const IfsFsHostRuntimeV2 *h)
{
    uint64_t seconds=h->realtime_ns?h->realtime_ns(h->context)/1000000000ull:0u;
    return seconds<=UINT32_MAX-2082844800u?(uint32_t)seconds+2082844800u:2082844800u;
}
/* Publish a dirty MDB before modifying any allocation metadata. On an I/O
 * failure restore the prior metadata; if rollback fails, never mark the
 * volume clean and refuse further mutations on the lease. No HFS journal
 * or power-loss atomicity is claimed. */
static int mutation_commit(HfsMutation *m)
{
    HfsMount *slot=m->slot;
    const IfsFsHostRuntimeV2 *h=&slot->host;
    const uint64_t alt=h->volume_bytes-1024u,tree_at=HFS_FMT_HEAP+m->unit;
    uint8_t dirty[512];memcpy(dirty,m->oldmdb,sizeof(dirty));store16(dirty+10u,0u);
    if(be32(m->mdb+70u)==UINT32_MAX)return IFS_FS_NO_SPACE;
    store32(m->mdb+6u,mutation_time(h));
    store32(m->mdb+70u,be32(m->mdb+70u)+1u);
    int rc=format_write(h,1024u,dirty,sizeof(dirty));
    if(rc==IFS_FS_OK)rc=format_write(h,alt,dirty,sizeof(dirty));
    if(rc==IFS_FS_OK && h->flush(h->context))rc=IFS_FS_IO_ERROR;
    if(rc==IFS_FS_OK)rc=format_write(h,tree_at,m->tree,m->unit);
    if(rc==IFS_FS_OK)rc=format_write(h,1536u,m->bitmap,HFS_FMT_BITMAP);
    if(rc==IFS_FS_OK && h->flush(h->context))rc=IFS_FS_IO_ERROR;
    if(rc==IFS_FS_OK)rc=format_write(h,alt,m->mdb,sizeof(m->mdb));
    if(rc==IFS_FS_OK)rc=format_write(h,1024u,m->mdb,sizeof(m->mdb));
    if(rc==IFS_FS_OK && h->flush(h->context))rc=IFS_FS_IO_ERROR;
    if(rc!=IFS_FS_OK) {
        int bad=0;
        /* Restore old allocation state before restoring either clean MDB. */
        if(ifs_fs_host_write_bytes_v2(h,1024u,dirty,sizeof(dirty))!=IFS_FS_OK)bad=1;
        if(ifs_fs_host_write_bytes_v2(h,alt,dirty,sizeof(dirty))!=IFS_FS_OK)bad=1;
        if(h->flush(h->context))bad=1;
        if(ifs_fs_host_write_bytes_v2(h,tree_at,m->oldtree,m->unit)!=IFS_FS_OK)bad=1;
        if(ifs_fs_host_write_bytes_v2(h,1536u,m->oldbitmap,HFS_FMT_BITMAP)!=IFS_FS_OK)bad=1;
        if(h->flush(h->context))bad=1;
        if(!bad) {
            if(ifs_fs_host_write_bytes_v2(h,alt,m->oldmdb,sizeof(m->oldmdb))!=IFS_FS_OK)bad=1;
            if(ifs_fs_host_write_bytes_v2(h,1024u,m->oldmdb,sizeof(m->oldmdb))!=IFS_FS_OK)bad=1;
            if(h->flush(h->context))bad=1;
        }
        if(bad) {
            slot->mutation_fault=1u;
            (void)ifs_fs_host_write_bytes_v2(h,1024u,dirty,sizeof(dirty));
            (void)ifs_fs_host_write_bytes_v2(h,alt,dirty,sizeof(dirty));
            (void)h->flush(h->context);
        }
        return rc;
    }
    memcpy(slot->catalog.allocation_bitmap,m->bitmap,m->bitmap_bytes);
    slot->catalog.first_leaf=be32(m->tree+24u);
    slot->info.free_bytes=(uint64_t)be16(m->mdb+34u)*m->unit;
    return IFS_FS_OK;
}
static int root_name(const char *path,const char **name,uint32_t *n)
{
    if(!path || path[0]!='/')return IFS_FS_INVALID;
    *name=path+1;size_t len=strlen(*name);
    if(len>31u || !name_supported(*name,(uint32_t)len))return IFS_FS_UNSUPPORTED;
    *n=(uint32_t)len;return IFS_FS_OK;
}
static int create_file(void *volume,const char *path,uint32_t type)
{
    HfsMount *slot;int rc=acquire_mount(volume,&slot);
    if(rc!=IFS_FS_OK)return rc;
    HfsMutation m={0};const char *name=NULL;uint32_t n=0u;
    if(!slot->writable)return release_mount(slot,IFS_FS_ACCESS);
    if(type!=IFS_FS_ENTRY_FILE)return release_mount(slot,IFS_FS_UNSUPPORTED);
    rc=root_name(path,&name,&n);if(rc!=IFS_FS_OK)return release_mount(slot,rc);
    rc=mutation_load(&m,slot);if(rc!=IFS_FS_OK)goto done;
    uint32_t at=2u;
    for(;at<m.count;++at) {
        int c=compare_name(&m.records[at],name,n);
        if(!c){rc=IFS_FS_EXISTS;goto done;}
        if(c>0)break;
    }
    uint32_t cnid=be32(m.mdb+30u);
    if(m.count==HFS_MUT_RECORDS || cnid<16u || cnid==UINT32_MAX) {
        rc=IFS_FS_NO_SPACE;goto done;
    }
    for(uint32_t j=m.count;j>at;--j)m.records[j]=m.records[j-1u];
    HfsRecord *r=&m.records[at];memset(r,0,sizeof(*r));
    uint32_t key=format_key(r->data,2u,name,n);r->length=key+102u;
    uint8_t *d=record_value(r);d[0]=2u;store32(d+20u,cnid);
    uint32_t now=mutation_time(&slot->host);store32(d+44u,now);store32(d+48u,now);
    ++m.count;store32(m.mdb+30u,cnid+1u);
    store16(m.mdb+12u,m.count-2u);store32(m.mdb+84u,m.count-2u);
    store16(record_value(&m.records[0])+4u,m.count-2u);
    rc=rebuild_catalogue(&m);
    if(rc==IFS_FS_OK)rc=mutation_commit(&m);
done:
    mutation_free(&m);return release_mount(slot,rc);
}
/* Allocate one next block from the native free bitmap. Extend the last
 * contiguous extent whenever possible; otherwise use the next free run.
 * An unrepresentable fourth inline extent fails before any data write. */
static int grow_fork(HfsMutation *m,uint8_t *d,uint32_t desired)
{
    uint32_t total=be32(d+30u)/m->unit;
    while(total<desired) {
        unsigned last=0u;
        while(last<3u && be16(d+76u+4u*last))++last;
        uint32_t block=m->blocks;
        if(last) {
            uint32_t b=be16(d+74u+4u*(last-1u))+be16(d+76u+4u*(last-1u));
            if(b<m->blocks && !used_block(m->bitmap,b))block=b;
        }
        if(block==m->blocks) {
            if(last==3u)return IFS_FS_NO_SPACE;
            for(uint32_t b=2u;b<m->blocks;++b)
                if(!used_block(m->bitmap,b)){block=b;break;}
            if(block==m->blocks)return IFS_FS_NO_SPACE;
            store16(d+74u+4u*last,block);
            store16(d+76u+4u*last,1u);
        } else {
            uint8_t *count=d+76u+4u*(last-1u);
            if(be16(count)==UINT16_MAX)return IFS_FS_NO_SPACE;
            store16(count,be16(count)+1u);
        }
        if(!be16(m->mdb+34u))return IFS_FS_CORRUPT;
        m->bitmap[block/8u]|=(uint8_t)(0x80u>>(block%8u));
        store16(m->mdb+34u,be16(m->mdb+34u)-1u);
        store16(m->mdb+16u,block+1u<m->blocks?block+1u:2u);
        ++total;
    }
    store16(d+24u,be16(d+74u));store32(d+30u,total*m->unit);
    return IFS_FS_OK;
}
static int write_file(void *volume,const char *path,uint64_t offset,
                       const void *data,uint32_t length)
{
    HfsMount *slot;int rc=acquire_mount(volume,&slot);
    if(rc!=IFS_FS_OK)return rc;
    if(!slot->writable)return release_mount(slot,IFS_FS_ACCESS);
    if(!data || !length || length>65536u || offset>UINT32_MAX-length)
        return release_mount(slot,IFS_FS_UNSUPPORTED);
    HfsMutation m={0};const char *name=NULL;uint32_t n=0u;
    rc=root_name(path,&name,&n);if(rc!=IFS_FS_OK)return release_mount(slot,rc);
    rc=mutation_load(&m,slot);if(rc!=IFS_FS_OK)goto done;
    uint8_t *d=NULL;
    for(uint32_t j=2u;j<m.count;++j)
        if(!compare_name(&m.records[j],name,n)){d=record_value(&m.records[j]);break;}
    if(!d){rc=IFS_FS_NOT_FOUND;goto done;}
    if(offset!=be32(d+26u)){rc=IFS_FS_UNSUPPORTED;goto done;}
    const uint64_t end=offset+length;
    uint64_t desired=(end+m.unit-1u)/m.unit;
    if(desired>m.blocks || desired*m.unit>UINT32_MAX){rc=IFS_FS_NO_SPACE;goto done;}
    rc=grow_fork(&m,d,(uint32_t)desired);if(rc!=IFS_FS_OK)goto done;
    store32(d+26u,(uint32_t)end);store32(d+48u,mutation_time(&slot->host));
    rc=rebuild_catalogue(&m);if(rc!=IFS_FS_OK)goto done;
    const IfsFsHostRuntimeV2 *h=&slot->host;
    uint8_t *zero=h->allocate(h->context,m.unit);
    if(!zero){rc=IFS_FS_NO_MEMORY;goto done;}
    memset(zero,0,m.unit);
    /* Unreferenced new blocks can be initialized before metadata commit.
     * Appends into an existing partial block change bytes beyond old EOF
     * only. No committed file content is overwritten by a failed append. */
    uint32_t remaining=length;const uint8_t *src=data;
    uint64_t base=0u,pos=offset;
    for(unsigned k=0u;k<3u && remaining;++k) {
        uint32_t start=be16(d+74u+4u*k),count=be16(d+76u+4u*k);
        uint64_t extent_bytes=(uint64_t)count*m.unit;
        if(!count || pos>=base+extent_bytes){base+=extent_bytes;continue;}
        while(remaining && pos<base+extent_bytes) {
            uint64_t relative=pos-base;
            uint32_t block=start+(uint32_t)(relative/m.unit);
            uint32_t skip=(uint32_t)(relative%m.unit);
            uint32_t take=m.unit-skip;if(take>remaining)take=remaining;
            uint64_t at=HFS_FMT_HEAP+(uint64_t)block*m.unit;
            if(!used_block(m.oldbitmap,block) && (skip || take<m.unit)) {
                rc=format_write(h,at,zero,m.unit);if(rc!=IFS_FS_OK)break;
            }
            rc=format_write(h,at+skip,src,take);if(rc!=IFS_FS_OK)break;
            pos+=take;src+=take;remaining-=take;
        }
        if(rc!=IFS_FS_OK)break;
        base+=extent_bytes;
    }
    h->release(h->context,zero);
    if(rc==IFS_FS_OK && remaining)rc=IFS_FS_CORRUPT;
    if(rc==IFS_FS_OK && h->flush(h->context))rc=IFS_FS_IO_ERROR;
    if(rc==IFS_FS_OK)rc=mutation_commit(&m);
done:
    mutation_free(&m);return release_mount(slot,rc);
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

/* Classic HFS has no universal Unicode label encoding. Qualify the
 * ASCII subset rather than misreport a script-specific name as UTF-8. */
static int volume_label(void *volume,char *out,uint32_t cap,uint32_t *bytes)
{
    if(!bytes)return IFS_FS_INVALID;
    *bytes=0u;
    if(!out || !cap)return IFS_FS_INVALID;
    HfsMount *slot;
    int rc=acquire_mount(volume,&slot);
    if(rc!=IFS_FS_OK)return rc;
    uint32_t n=slot->catalog.label_length;
    for(uint32_t i=0;i<n;++i) {
        uint8_t c=slot->catalog.label[i];
        if(c<32u || c==127u)return release_mount(slot,IFS_FS_CORRUPT);
        if(c>=128u)return release_mount(slot,IFS_FS_UNSUPPORTED);
    }
    if(n>=cap)return release_mount(slot,IFS_FS_NO_SPACE);
    memcpy(out,slot->catalog.label,n);out[n]=0;*bytes=n;
    return release_mount(slot,IFS_FS_OK);
}
static const IfsFsVolumeOpsV2 volume_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET
#ifndef IFS_HFS_FIRMWARE_BUILD
             |IFS_FS_VOLUME_FORMAT
#endif
    },
    .query=query_volume,.label=volume_label
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
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag_ops.header;
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
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) {return &api;}

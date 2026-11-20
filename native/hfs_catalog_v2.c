/* SPDX-License-Identifier: GPL-3.0-or-later
 * Classic HFS catalogue B-tree and fork extents, read-only Common V2.
 * Disk offsets and record layouts follow the original HFS volume format.
 * Supports catalogues and data forks represented by their inline three
 * extents; extent overflow and unsupported names fail closed.
 */
#include "hfs_catalog_v2.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define HFS_MAX_NODE_BYTES 32768u
#define HFS_ROOT_ID 2u
#define HFS_RECORD_FILE 2u
#define HFS_RECORD_DIRECTORY 1u

typedef struct {
    uint32_t parent, id, length;
    uint8_t type, name_length;
    uint8_t name[31];
    HfsExtentV2 extents[3];
} CataloguedEntry;

static uint16_t be16(const uint8_t *p)
{ return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) |
         ((uint32_t)p[2]<<8) | p[3]; }

static int media_read(const IfsFsHostRuntimeV2 *host, uint64_t offset,
                      void *buffer, uint32_t bytes)
{
    if (!host || !buffer || offset>host->volume_bytes ||
        bytes>host->volume_bytes-offset) return IFS_FS_CORRUPT;
    return ifs_fs_host_read_bytes_v2(host, offset, buffer, bytes);
}

static int checked_extent(const HfsCatalogueV2 *volume,
                          const HfsExtentV2 *extent)
{
    if (extent->block_start>volume->allocation_blocks ||
        extent->block_count>volume->allocation_blocks-extent->block_start)
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int fork_read(const HfsCatalogueV2 *v, const HfsExtentV2 extents[3],
                     uint64_t logical_bytes, uint64_t offset,
                     void *buffer, uint32_t bytes)
{
    uint64_t end;
    uint8_t *out = buffer;
    if (!v || (!buffer && bytes) || offset>logical_bytes ||
        bytes>logical_bytes-offset) return IFS_FS_INVALID;
    if (!bytes) return IFS_FS_OK;
    end = offset + bytes;
    uint64_t base = 0;
    for (unsigned i=0; i<3u; ++i) {
        const HfsExtentV2 *ext = &extents[i];
        if (checked_extent(v,ext)!=IFS_FS_OK) return IFS_FS_CORRUPT;
        uint64_t len = (uint64_t)ext->block_count*v->allocation_bytes;
        if (base<=offset && offset<base+len) {
            uint64_t take = len-(offset-base);
            if (take>end-offset) take=end-offset;
            uint64_t position = v->allocation_start +
                (uint64_t)ext->block_start*v->allocation_bytes+(offset-base);
            int rc = media_read(v->host,position,out,(uint32_t)take);
            if (rc!=IFS_FS_OK) return rc;
            out += take;
            offset += take;
            if (offset==end) return IFS_FS_OK;
        }
        if (base>UINT64_MAX-len) return IFS_FS_CORRUPT;
        base += len;
    }
    return IFS_FS_UNSUPPORTED; /* needs the HFS extent overflow B-tree */
}

static int tree_read(const HfsCatalogueV2 *v, uint64_t off,
                     void *out, uint32_t bytes)
{
    return fork_read(v,v->catalogue_extents,v->catalogue_size,off,out,bytes);
}
static int bit_used(const HfsCatalogueV2 *v, uint32_t block)
{
    /* Classic HFS allocation bits are MSB-first and partition-relative. */
    if (block>=v->allocation_blocks) return IFS_FS_CORRUPT;
    uint8_t value=0;
    uint8_t mdb[512];
    int rc=media_read(v->host,1024u,mdb,sizeof(mdb));
    if (rc!=IFS_FS_OK) return rc;
    uint64_t bitmap=(uint64_t)be16(mdb+14u)*512u+(block>>3u);
    rc=media_read(v->host,bitmap,&value,1u);
    if (rc!=IFS_FS_OK) return rc;
    return (value & (0x80u>>(block&7u))) ? IFS_FS_OK : IFS_FS_CORRUPT;
}
static int check_allocated_extents(const HfsCatalogueV2 *v,
                                   const HfsExtentV2 ext[3],
                                   uint64_t used_bytes)
{
    uint64_t span=0;
    for (unsigned i=0;i<3;++i) {
        if (checked_extent(v,&ext[i])!=IFS_FS_OK) return IFS_FS_CORRUPT;
        uint64_t size=(uint64_t)ext[i].block_count*v->allocation_bytes;
        for (uint32_t b=0;b<ext[i].block_count;++b) {
            int rc=bit_used(v,ext[i].block_start+b);
            if (rc!=IFS_FS_OK) return rc;
        }
        if (span>UINT64_MAX-size) return IFS_FS_CORRUPT;
        span+=size;
    }
    return used_bytes>span ? IFS_FS_UNSUPPORTED : IFS_FS_OK;
}
int hfs_catalogue_open_v2(const IfsFsHostRuntimeV2 *host, HfsCatalogueV2 *v)
{
    uint8_t mdb[512], header[64];
    if (!host || !v || !ifs_fs_host_v2_valid(host)) return IFS_FS_INVALID;
    memset(v,0,sizeof(*v));
    v->host=host;
    int rc=media_read(host,1024u,mdb,sizeof(mdb));
    if (rc!=IFS_FS_OK) return rc;
    if (be16(mdb)!=0x4244u) return IFS_FS_NOT_FOUND;
    v->allocation_start=(uint64_t)be16(mdb+28u)*512u;
    v->allocation_bytes=be32(mdb+20u);
    v->allocation_blocks=be16(mdb+18u);
    v->catalogue_size=be32(mdb+146u);
    if (!v->allocation_bytes || v->allocation_bytes%512u ||
        !v->allocation_blocks || v->catalogue_size<512u ||
        v->catalogue_size>host->volume_bytes ||
        v->allocation_start>host->volume_bytes) return IFS_FS_CORRUPT;
    for (unsigned i=0;i<3u;++i) {
        v->catalogue_extents[i].block_start=be16(mdb+150u+4u*i);
        v->catalogue_extents[i].block_count=be16(mdb+152u+4u*i);
    }
    rc=check_allocated_extents(v,v->catalogue_extents,v->catalogue_size);
    if (rc!=IFS_FS_OK) return rc;
    rc=tree_read(v,0u,header,sizeof(header));
    if (rc!=IFS_FS_OK) return rc;
    if (header[8]!=1u || be16(header+10u)<1u)
        return IFS_FS_CORRUPT;
    v->node_bytes=be16(header+32u);
    v->node_count=be32(header+36u);
    v->first_leaf=be32(header+24u);
    if (v->node_bytes<512u || v->node_bytes>HFS_MAX_NODE_BYTES ||
        (v->node_bytes&(v->node_bytes-1u)) ||
        !v->node_count ||
        (uint64_t)v->node_count*v->node_bytes>v->catalogue_size ||
        (v->first_leaf && v->first_leaf>=v->node_count))
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int parse_entry(const uint8_t *node, uint32_t size,
                       uint32_t start, uint32_t finish,
                       CataloguedEntry *entry)
{
    if (start<14u || finish>size || start>=finish ||
        finish-start<8u) return IFS_FS_CORRUPT;
    const uint8_t *key=node+start;
    uint8_t key_length=key[0];
    if (key_length<6u || key_length>37u ||
        (uint32_t)key_length+1u>finish-start)
        return IFS_FS_CORRUPT;
    uint8_t length=key[6];
    /* HFS catalogue threads encode an empty Pascal name as a 7-byte
     * search key. Reject arbitrary key-length mismatches but preserve
     * the native root/file thread record, which is not a directory child. */
    const int empty_thread_key = (length==0u && key_length==7u);
    if (length>31u ||
        ((uint32_t)key_length!=6u+length && !empty_thread_key))
        return IFS_FS_CORRUPT;
    uint32_t data_off=start+((uint32_t)(key_length+2u)&~1u);
    if (data_off>=finish) return IFS_FS_CORRUPT;
    memset(entry,0,sizeof(*entry));
    entry->parent=be32(key+2u);
    entry->name_length=length;
    memcpy(entry->name,key+7u,length);
    const uint8_t *record=node+data_off;
    const uint32_t available=finish-data_off;
    if (empty_thread_key && record[0]!=3u && record[0]!=4u)
        return IFS_FS_CORRUPT;
    if (record[0]==HFS_RECORD_DIRECTORY) {
        if (available<70u) return IFS_FS_CORRUPT;
        entry->type=IFS_FS_ENTRY_DIRECTORY;
        entry->id=be32(record+6u);
        if (entry->id<2u) return IFS_FS_CORRUPT;
    } else if (record[0]==HFS_RECORD_FILE) {
        if (available<102u) return IFS_FS_CORRUPT;
        entry->type=IFS_FS_ENTRY_FILE;
        entry->id=be32(record+20u);
        entry->length=be32(record+26u);
        if (entry->id<16u) return IFS_FS_CORRUPT;
        for (unsigned i=0;i<3;++i) {
            entry->extents[i].block_start=be16(record+74u+4u*i);
            entry->extents[i].block_count=be16(record+76u+4u*i);
        }
    } else if (record[0]==3u || record[0]==4u) {
        entry->type=0u; /* catalogue thread, not a child */
    } else {
        return IFS_FS_CORRUPT;
    }
    return IFS_FS_OK;
}
typedef struct {
    uint32_t parent;
    const char *name;
    uint32_t name_length, ordinal, encountered;
    CataloguedEntry result;
    int found;
} Scan;
static int key_match(const CataloguedEntry *record, const Scan *scan)
{
    if (record->parent!=scan->parent || !record->type) return 0;
    if (!scan->name) return 1;
    if (scan->name_length!=record->name_length) return 0;
    for (uint32_t i=0;i<record->name_length;++i) {
        unsigned char a=record->name[i], b=(unsigned char)scan->name[i];
        if (a>='A'&&a<='Z') a=(unsigned char)(a+32);
        if (b>='A'&&b<='Z') b=(unsigned char)(b+32);
        if (a!=b) return 0;
    }
    return 1;
}
static int catalogue_scan(const HfsCatalogueV2 *v, Scan *scan)
{
    if (!v || !scan || !v->host) return IFS_FS_INVALID;
    if (!v->first_leaf) return IFS_FS_NOT_FOUND;
    uint8_t *node=v->host->allocate(v->host->context,v->node_bytes);
    if (!node) return IFS_FS_NO_MEMORY;
    uint32_t current=v->first_leaf, previous=0u;
    int result=IFS_FS_NOT_FOUND;
    for (uint32_t visit=0u; visit<v->node_count; ++visit) {
        result=tree_read(v,(uint64_t)current*v->node_bytes,node,v->node_bytes);
        if (result!=IFS_FS_OK) break;
        uint16_t count=be16(node+10u);
        if (node[8]!=0xffu || be32(node+4u)!=previous ||
            (uint32_t)(count+1u)*2u>v->node_bytes-14u) {
            result=IFS_FS_CORRUPT; break;
        }
        for (uint32_t index=0u;index<count;++index) {
            uint32_t begin=be16(node+v->node_bytes-2u*(index+1u));
            uint32_t finish=be16(node+v->node_bytes-2u*(index+2u));
            if (finish>v->node_bytes-2u*(count+1u) ||
                begin>=finish) {result=IFS_FS_CORRUPT;goto done;}
            CataloguedEntry record;
            result=parse_entry(node,v->node_bytes,begin,finish,&record);
            if (result!=IFS_FS_OK) goto done;
            if (key_match(&record,scan)) {
                if (scan->name || scan->encountered++==scan->ordinal) {
                    scan->result=record;scan->found=1;
                    result=IFS_FS_OK;goto done;
                }
            }
        }
        uint32_t next=be32(node);
        if (!next) {result=IFS_FS_NOT_FOUND;goto done;}
        if (next>=v->node_count || next==current) {
            result=IFS_FS_CORRUPT;goto done;
        }
        previous=current;current=next;
    }
    if (result==IFS_FS_OK) result=IFS_FS_CORRUPT; /* cyclic chain */
done:
    v->host->release(v->host->context,node);
    return result;
}
static int resolve(const HfsCatalogueV2 *v, const char *path,
                   CataloguedEntry *result)
{
    if (!path || *path!='/') return IFS_FS_INVALID;
    memset(result,0,sizeof(*result));
    result->type=IFS_FS_ENTRY_DIRECTORY;
    result->id=HFS_ROOT_ID;
    if (!path[1]) return IFS_FS_OK;
    uint32_t parent=HFS_ROOT_ID;
    const char *segment=path+1;
    for (uint32_t depth=0;depth<64u;++depth) {
        const char *end=segment;
        while (*end && *end!='/') ++end;
        size_t length=(size_t)(end-segment);
        if (length==0u || length>31u ||
            (length==1u && segment[0]=='.') ||
            (length==2u && segment[0]=='.'&&segment[1]=='.'))
            return IFS_FS_INVALID;
        for (size_t i=0;i<length;++i) {
            if ((unsigned char)segment[i]<32u ||
                (unsigned char)segment[i]>126u ||
                segment[i]==':') return IFS_FS_UNSUPPORTED;
        }
        Scan scan={.parent=parent,.name=segment,
                   .name_length=(uint32_t)length};
        int rc=catalogue_scan(v,&scan);
        if (rc!=IFS_FS_OK) return rc;
        *result=scan.result;
        if (!*end) return IFS_FS_OK;
        if (result->type!=IFS_FS_ENTRY_DIRECTORY) return IFS_FS_NOT_FOUND;
        parent=result->id;segment=end+1;
    }
    return IFS_FS_INVALID;
}
static int entry_out(const CataloguedEntry *record, IfsFsEntryV2 *entry)
{
    if (!entry || entry->structure_bytes<sizeof(*entry) || !entry->name)
        return IFS_FS_INVALID;
    uint32_t length=record->name_length;
    if (length>=entry->name_capacity) return IFS_FS_NO_SPACE;
    for (uint32_t i=0;i<length;++i) {
        if (record->name[i]<32u || record->name[i]>126u)
            return IFS_FS_UNSUPPORTED; /* MacRoman UTF-8 mapping pending */
    }
    memcpy(entry->name,record->name,length);
    entry->name[length]='\0';
    entry->name_bytes=length;
    entry->size_bytes=record->type==IFS_FS_ENTRY_FILE?record->length:0u;
    entry->type=record->type;
    entry->attributes=0u;
    return IFS_FS_OK;
}
int hfs_catalogue_stat_v2(const HfsCatalogueV2 *v, const char *path,
                          IfsFsEntryV2 *entry)
{
    CataloguedEntry item;
    int rc=resolve(v,path,&item);
    if (rc!=IFS_FS_OK) return rc;
    if (!path[1]) {
        item.name[0]='/';item.name_length=1u;
    }
    return entry_out(&item,entry);
}
int hfs_catalogue_list_v2(const HfsCatalogueV2 *v, const char *path,
                          uint32_t index, IfsFsEntryV2 *entry)
{
    CataloguedEntry directory;
    int rc=resolve(v,path,&directory);
    if (rc!=IFS_FS_OK) return rc;
    if (directory.type!=IFS_FS_ENTRY_DIRECTORY) return IFS_FS_INVALID;
    Scan scan={.parent=directory.id,.ordinal=index};
    rc=catalogue_scan(v,&scan);
    if (rc==IFS_FS_NOT_FOUND) return IFS_FS_END;
    if (rc!=IFS_FS_OK) return rc;
    return entry_out(&scan.result,entry);
}
int hfs_catalogue_read_v2(const HfsCatalogueV2 *v, const char *path,
                          uint64_t offset, void *output, uint32_t bytes)
{
    CataloguedEntry file;
    int rc=resolve(v,path,&file);
    if (rc!=IFS_FS_OK) return rc;
    if (file.type!=IFS_FS_ENTRY_FILE) return IFS_FS_INVALID;
    rc=check_allocated_extents(v,file.extents,file.length);
    if (rc!=IFS_FS_OK) return rc;
    return fork_read(v,file.extents,file.length,offset,output,bytes);
}

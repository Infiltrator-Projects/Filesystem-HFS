/* SPDX-License-Identifier: GPL-3.0-or-later
 * Classic HFS hosted relocation for the independently qualified native
 * flat-root profile. The native formatter owns the otherwise unused
 * boot area. A checked redo record there authorizes the exact change
 * of one catalogue data fork. No foreign boot sector is appropriated.
 *
 * New data is copied, verified and durably flushed before publishing
 * intent. Allocation bitmap destination bits become durable before
 * switching the catalogue record; old bits are cleared only afterwards.
 * The MDB free count is unchanged by a move. On a fresh mount, replay
 * the intent before validating the MDB against the bitmap. Only the
 * native data-fork record is changed, without restructuring its B-tree.
 */
#define HR_BYTES 128u
static const uint8_t hr_magic[8]={'I','F','S','H','R','V','2',0};
typedef struct {
    uint32_t cnid,offset,size,count,target,reserve;
    uint16_t start[3],length[3];
} HrFile;
typedef struct {
    uint32_t count,policy,executed;
    HrFile file[HFS_MUT_FILES];
} HrPlan;
typedef struct {
    uint32_t unit,blocks,created;
    uint64_t volume_bytes;
} HrGeometry;
static uint32_t hr_checksum(const uint8_t *p)
{
    uint32_t h=UINT32_C(2166136261);
    for(unsigned i=0;i<HR_BYTES-4u;++i)
        h=(h^p[i])*UINT32_C(16777619);
    return h;
}
static int hr_read(const IfsFsHostRuntimeV2 *h,uint64_t at,void *p,uint32_t n)
{ return ifs_fs_host_read_bytes_v2(h,at,p,n); }
static int hr_write(const IfsFsHostRuntimeV2 *h,uint64_t at,const void *p,uint32_t n)
{ return ifs_fs_host_write_bytes_v2(h,at,p,n); }
static int hr_flush(const IfsFsHostRuntimeV2 *h)
{ return h->flush(h->context)==0?IFS_FS_OK:IFS_FS_IO_ERROR; }
static int hr_host_writable(const IfsFsHostRuntimeV2 *h)
{
    return !(h->capabilities&IFS_FS_HOST_READ_ONLY) &&
        (h->capabilities&IFS_FS_HOST_EXCLUSIVE) &&
        (h->capabilities&IFS_FS_HOST_DURABLE) && h->write_at && h->flush;
}
static int hr_cancelled(const IfsFsHostRuntimeV2 *h)
{ return h->cancelled && h->cancelled(h->context); }
static void hr_encode(uint8_t raw[HR_BYTES],const HrGeometry *g,const HrFile *f)
{
    memset(raw,0,HR_BYTES);memcpy(raw,hr_magic,sizeof hr_magic);
    store32(raw+8,g->unit);store32(raw+12,g->blocks);store32(raw+16,g->created);
    store32(raw+20,(uint32_t)(g->volume_bytes>>32));
    store32(raw+24,(uint32_t)g->volume_bytes);
    if(f) {
        store32(raw+28,1u);store32(raw+32,f->cnid);store32(raw+36,f->offset);
        store32(raw+40,f->size);store32(raw+44,f->count);store32(raw+48,f->target);
        for(unsigned k=0;k<3u;++k) {
            store16(raw+52+4u*k,f->start[k]);store16(raw+54+4u*k,f->length[k]);
        }
    }
    store32(raw+HR_BYTES-4u,hr_checksum(raw));
}
static int hr_file_valid(const HrGeometry *g,const HrFile *f)
{
    if(f->cnid<16u || !f->size || !f->count || f->count>UINT16_MAX ||
       f->target<2u || f->target>=g->blocks || f->count>g->blocks-f->target ||
       f->offset>=g->unit || 102u>g->unit-f->offset ||
       ((uint64_t)f->size+g->unit-1u)/g->unit!=f->count)
        return 0;
    uint32_t total=0u;int ended=0;
    for(unsigned k=0;k<3u;++k) {
        const uint32_t b=f->start[k],n=f->length[k];
        if(!n){if(b)return 0;ended=1;continue;}
        if(ended || b<2u || b>=g->blocks || n>g->blocks-b ||
           (b<f->target+f->count && f->target<b+n))return 0;
        for(unsigned j=0;j<k;++j)
            if(b<(uint32_t)f->start[j]+f->length[j] &&
               f->start[j]<b+n)return 0;
        total+=n;
    }
    return total==f->count;
}
static int hr_journal_read(const IfsFsHostRuntimeV2 *h,HrGeometry *g,HrFile *f,int *state)
{
    *state=0;
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(h->logical_sector_bytes>1024u || h->volume_bytes<HFS_FMT_HEAP+8192u)
        return IFS_FS_OK;
    uint8_t raw[HR_BYTES],mdb[512];
    int rc=hr_read(h,0u,raw,sizeof raw);
    if(rc!=IFS_FS_OK)return rc;
    if(memcmp(raw,hr_magic,sizeof hr_magic))return IFS_FS_OK;
    if(be32(raw+HR_BYTES-4u)!=hr_checksum(raw) || be32(raw+28)>1u)
        return IFS_FS_CORRUPT;
    rc=hr_read(h,1024u,mdb,sizeof mdb);
    if(rc!=IFS_FS_OK)return rc;
    *g=(HrGeometry){be32(raw+8),be32(raw+12),be32(raw+16),
        ((uint64_t)be32(raw+20)<<32)|be32(raw+24)};
    if(be16(mdb)!=0x4244u || be16(mdb+10)!=0x100u ||
       be16(mdb+14)!=3u || be16(mdb+28)!=HFS_FMT_HEAP/512u ||
       g->unit<4096u || g->unit>32768u || (g->unit&(g->unit-1u)) ||
       g->unit!=be32(mdb+20) || g->blocks!=be16(mdb+18) ||
       g->created!=be32(mdb+2) || g->volume_bytes!=h->volume_bytes ||
       g->blocks<2u || (uint64_t)g->blocks*g->unit>h->volume_bytes-HFS_FMT_HEAP-1024u ||
       be32(mdb+146)!=g->unit || be16(mdb+150)!=1u ||
       be16(mdb+152)!=1u || be16(mdb+154) || be16(mdb+156) ||
       be16(mdb+158) || be16(mdb+160))
        return IFS_FS_CORRUPT;
    *state=be32(raw+28)?2:1;
    if(*state==2) {
        memset(f,0,sizeof *f);
        f->cnid=be32(raw+32);f->offset=be32(raw+36);
        f->size=be32(raw+40);f->count=be32(raw+44);f->target=be32(raw+48);
        for(unsigned k=0;k<3u;++k) {
            f->start[k]=be16(raw+52+4u*k);f->length[k]=be16(raw+54+4u*k);
        }
        if(!hr_file_valid(g,f))return IFS_FS_CORRUPT;
    }
    return IFS_FS_OK;
}
static int hr_journal_write(const IfsFsHostRuntimeV2 *h,const HrGeometry *g,const HrFile *f)
{
    uint8_t raw[HR_BYTES];hr_encode(raw,g,f);
    int rc=hr_write(h,0u,raw,sizeof raw);
    return rc!=IFS_FS_OK?rc:hr_flush(h);
}
/* Match the journal's catalogue offset against the actual live leaf
 * record and CNID, not merely an unchecked byte position in metadata. */
static int hr_record(const IfsFsHostRuntimeV2 *h,const HrGeometry *g,const HrFile *f,
                     uint8_t leaf[512],uint8_t **value,uint64_t *disk_at,int *is_new)
{
    const uint32_t node=f->offset/512u,off=f->offset%512u;
    if(!node || node>=g->unit/512u)return IFS_FS_CORRUPT;
    *disk_at=HFS_FMT_HEAP+(uint64_t)g->unit+node*512u;
    int rc=hr_read(h,*disk_at,leaf,512u);
    if(rc!=IFS_FS_OK)return rc;
    const uint32_t n=be16(leaf+10u);
    if(leaf[8]!=0xffu || leaf[9]!=1u || !n || n>248u)return IFS_FS_CORRUPT;
    *value=NULL;
    for(uint32_t j=0;j<n;++j) {
        const uint32_t a=be16(leaf+510u-2u*j),b=be16(leaf+508u-2u*j);
        if(a<14u || b<=a || b>512u-2u*(n+1u) || b-a<8u)return IFS_FS_CORRUPT;
        const uint32_t key=((uint32_t)leaf[a]+2u)&~1u;
        if(key>b-a)return IFS_FS_CORRUPT;
        if(a+key==off) {
            if(b-off!=102u || leaf[off]!=2u || be32(leaf+a+2u)!=2u ||
               be32(leaf+off+20u)!=f->cnid)return IFS_FS_CORRUPT;
            *value=leaf+off;break;
        }
    }
    if(!*value)return IFS_FS_CORRUPT;
    const uint8_t *d=*value;
    if(be32(d+26u)!=f->size || be32(d+30u)!=(uint64_t)f->count*g->unit ||
       be16(d+34u) || be32(d+36u) || be32(d+40u))return IFS_FS_CORRUPT;
    int old=be16(d+24u)==f->start[0],new=be16(d+24u)==f->target;
    for(unsigned k=0;k<3u;++k) {
        uint32_t b=be16(d+74u+4u*k),nblocks=be16(d+76u+4u*k);
        if(b!=f->start[k] || nblocks!=f->length[k])old=0;
        if(b!=(k?0u:f->target) || nblocks!=(k?0u:f->count))new=0;
    }
    if(old==new)return IFS_FS_CORRUPT;
    *is_new=new;
    return IFS_FS_OK;
}
static int hr_copy(const IfsFsHostRuntimeV2 *h,const HrGeometry *g,const HrFile *f,int copy)
{
    uint8_t *buf=h->allocate(h->context,2u*(size_t)g->unit);
    if(!buf)return IFS_FS_NO_MEMORY;
    uint32_t logical=0;int rc=IFS_FS_OK;
    for(unsigned k=0;k<3u && rc==IFS_FS_OK;++k)
        for(uint32_t i=0u;i<f->length[k] && rc==IFS_FS_OK;++i,++logical) {
            if(copy && hr_cancelled(h)){rc=IFS_FS_BUSY;break;}
            const uint64_t from=HFS_FMT_HEAP+(uint64_t)(f->start[k]+i)*g->unit;
            const uint64_t to=HFS_FMT_HEAP+(uint64_t)(f->target+logical)*g->unit;
            rc=hr_read(h,from,buf,g->unit);
            if(rc==IFS_FS_OK && copy)rc=hr_write(h,to,buf,g->unit);
            if(rc==IFS_FS_OK)rc=hr_read(h,to,buf+g->unit,g->unit);
            if(rc==IFS_FS_OK && memcmp(buf,buf+g->unit,g->unit))rc=IFS_FS_CORRUPT;
        }
    if(rc==IFS_FS_OK && copy)rc=hr_flush(h);
    h->release(h->context,buf);
    return rc;
}
static void hr_set_bit(uint8_t *map,uint32_t b,int value)
{
    const uint8_t mask=(uint8_t)(0x80u>>(b%8u));
    if(value)map[b/8u]|=mask;else map[b/8u]&=(uint8_t)~mask;
}
static int hr_replay(const IfsFsHostRuntimeV2 *h,const HrGeometry *g,const HrFile *f)
{
    uint8_t leaf[512],*d=NULL,bitmap[HFS_FMT_BITMAP];
    uint64_t disk_at;int is_new=0;
    int rc=hr_record(h,g,f,leaf,&d,&disk_at,&is_new);
    if(rc!=IFS_FS_OK)return rc;
    rc=hr_read(h,1536u,bitmap,sizeof bitmap);
    if(rc!=IFS_FS_OK)return rc;
    if(!is_new) {
        for(unsigned k=0;k<3u;++k)
            for(uint32_t i=0u;i<f->length[k];++i)
                if(!used_block(bitmap,f->start[k]+i))return IFS_FS_CORRUPT;
    } else {
        for(uint32_t i=0u;i<f->count;++i)
            if(!used_block(bitmap,f->target+i))return IFS_FS_CORRUPT;
    }
    rc=hr_copy(h,g,f,0);
    if(rc!=IFS_FS_OK)return rc;
    if(!is_new) {
        for(uint32_t i=0u;i<f->count;++i)hr_set_bit(bitmap,f->target+i,1);
        rc=hr_write(h,1536u,bitmap,sizeof bitmap);
        if(rc==IFS_FS_OK)rc=hr_flush(h);
        if(rc!=IFS_FS_OK)return rc;
        store16(d+24u,f->target);
        memset(d+74u,0,12u);store16(d+74u,f->target);store16(d+76u,f->count);
        rc=hr_write(h,disk_at,leaf,sizeof leaf);
        if(rc==IFS_FS_OK)rc=hr_flush(h);
        if(rc!=IFS_FS_OK)return rc;
    }
    for(unsigned k=0;k<3u;++k)
        for(uint32_t i=0u;i<f->length[k];++i)hr_set_bit(bitmap,f->start[k]+i,0);
    rc=hr_write(h,1536u,bitmap,sizeof bitmap);
    if(rc==IFS_FS_OK)rc=hr_flush(h);
    if(rc!=IFS_FS_OK)return rc;
    return hr_journal_write(h,g,NULL);
}
static int hr_mount_prepare(const IfsFsHostRuntimeV2 *h,unsigned *state,uint64_t *recovered)
{
    HrGeometry g;HrFile f;int found=0;
    *state=0u;*recovered=0u;
    int rc=hr_journal_read(h,&g,&f,&found);
    if(rc!=IFS_FS_OK)return rc;
    if(found==2) {
        if(!hr_host_writable(h))return IFS_FS_ACCESS;
        rc=hr_replay(h,&g,&f);
        if(rc!=IFS_FS_OK)return rc;
        *recovered=1u;found=1;
    }
    *state=(unsigned)found;return IFS_FS_OK;
}
static int hr_pending_probe(const IfsFsHostRuntimeV2 *h,int *pending)
{
    HrGeometry g;HrFile f;int state=0;
    int rc=hr_journal_read(h,&g,&f,&state);
    *pending=state==2;return rc;
}
static void hr_drop_plan(HfsMount *slot)
{
    if(slot->reloc_plan)slot->host.release(slot->host.context,slot->reloc_plan);
    slot->reloc_plan=NULL;slot->reloc_token=0u;
}
static int hr_plan(void *volume,const char *path,uint32_t policy,void **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    if(!path || (policy!=0u && policy!=10u))return IFS_FS_INVALID;
    HfsMount *slot;int rc=acquire_mount(volume,&slot);
    if(rc!=IFS_FS_OK)return rc;
    if(slot->reloc_state!=1u || slot->mutation_fault)
        return release_mount(slot,IFS_FS_UNSUPPORTED);
    if(slot->reloc_plan || slot->reloc_generation==UINTPTR_MAX)
        return release_mount(slot,IFS_FS_BUSY);
    const char *name=NULL;uint32_t name_bytes=0u;
    if(path[0]!='/' || path[1]) {
        rc=root_name(path,&name,&name_bytes);
        if(rc!=IFS_FS_OK)return release_mount(slot,rc);
    }
    HfsMutation m={0};HrPlan *p=NULL;
    rc=mutation_load(&m,slot);
    if(rc!=IFS_FS_OK)goto done;
    p=slot->host.allocate(slot->host.context,sizeof *p);
    if(!p){rc=IFS_FS_NO_MEMORY;goto done;}
    memset(p,0,sizeof *p);p->policy=policy;
    uint32_t cursor=2u;int found=name?0:1;
    for(uint32_t j=2u;j<m.count;++j) {
        HfsRecord *record=&m.records[j];uint8_t *d=record_value(record);
        if(name && compare_name(record,name,name_bytes))continue;
        found=1;
        if(hr_cancelled(&slot->host)){rc=IFS_FS_BUSY;goto done;}
        if(!be32(d+26u))continue;
        if(!policy && !be16(d+80u))continue;
        HrFile *f=&p->file[p->count++];
        f->cnid=be32(d+20u);f->offset=record->offset+record_key_bytes(record);
        f->size=be32(d+26u);f->count=be32(d+30u)/m.unit;
        f->reserve=policy?(f->count+9u)/10u:0u;
        for(unsigned k=0;k<3u;++k) {
            f->start[k]=be16(d+74u+4u*k);f->length[k]=be16(d+76u+4u*k);
        }
        const uint32_t needed=f->count+f->reserve;
        uint32_t run=0u;
        for(;cursor<m.blocks;++cursor) {
            if(hr_cancelled(&slot->host)){rc=IFS_FS_BUSY;goto done;}
            if(used_block(m.bitmap,cursor))run=0u;
            else if(++run>=needed){f->target=cursor-run+1u;++cursor;break;}
        }
        if(!f->target){rc=IFS_FS_NO_SPACE;goto done;}
    }
    if(!found){rc=IFS_FS_NOT_FOUND;goto done;}
    slot->reloc_plan=p;slot->reloc_token=++slot->reloc_generation;
    *out=(void *)slot->reloc_token;p=NULL;rc=IFS_FS_OK;
done:
    if(p)slot->host.release(slot->host.context,p);
    mutation_free(&m);return release_mount(slot,rc);
}
static HrPlan *hr_get(HfsMount *slot,void *token)
{ return token && (uintptr_t)token==slot->reloc_token?slot->reloc_plan:NULL; }
static int hr_refresh(HfsMount *slot)
{
    /* The native catalogue reads leaf records directly. Its cached bitmap
     * must be updated after each transaction before exposing any reads. */
    return hr_read(&slot->host,1536u,slot->catalog.allocation_bitmap,
                   slot->catalog.bitmap_bytes);
}
static int hr_execute(void *volume,void *token,uint64_t *moved)
{
    if(!moved)return IFS_FS_INVALID;
    *moved=0u;
    HfsMount *slot;int rc=acquire_mount(volume,&slot);
    if(rc!=IFS_FS_OK)return rc;
    HrPlan *p=hr_get(slot,token);
    if(!p || p->executed)return release_mount(slot,IFS_FS_INVALID);
    if(!slot->writable || !hr_host_writable(&slot->host))
        return release_mount(slot,IFS_FS_ACCESS);
    if(slot->reloc_state!=1u || slot->mutation_fault)
        return release_mount(slot,IFS_FS_IO_ERROR);
    const IfsFsHostRuntimeV2 *h=&slot->host;
    HrGeometry g;HrFile unused;int state=0;
    rc=hr_journal_read(h,&g,&unused,&state);
    if(rc!=IFS_FS_OK || state!=1)return release_mount(slot,rc!=IFS_FS_OK?rc:IFS_FS_CORRUPT);
    for(uint32_t j=0u;j<p->count;++j) {
        const HrFile *f=&p->file[j];
        if(hr_cancelled(h))return release_mount(slot,IFS_FS_BUSY);
        if(!hr_file_valid(&g,f))return release_mount(slot,IFS_FS_CORRUPT);
        uint8_t leaf[512],*d=NULL;uint64_t disk_at;int is_new=0;
        rc=hr_record(h,&g,f,leaf,&d,&disk_at,&is_new);
        if(rc!=IFS_FS_OK || is_new)return release_mount(slot,rc!=IFS_FS_OK?rc:IFS_FS_CORRUPT);
        for(uint32_t i=0u;i<f->count+f->reserve;++i)
            if(used_block(slot->catalog.allocation_bitmap,f->target+i))
                return release_mount(slot,IFS_FS_CORRUPT);
        rc=hr_copy(h,&g,f,1);
        if(rc!=IFS_FS_OK)return release_mount(slot,rc);
        slot->reloc_state=2u;
        rc=hr_journal_write(h,&g,f);
        if(rc==IFS_FS_OK)rc=hr_replay(h,&g,f);
        if(rc!=IFS_FS_OK)return release_mount(slot,rc);
        rc=hr_refresh(slot);
        if(rc!=IFS_FS_OK)return release_mount(slot,rc);
        slot->reloc_state=1u;*moved+=f->size;
    }
    p->executed=1u;return release_mount(slot,IFS_FS_OK);
}
static int hr_verify(void *volume,void *token)
{
    HfsMount *slot;int rc=acquire_mount(volume,&slot);
    if(rc!=IFS_FS_OK)return rc;
    HrPlan *p=hr_get(slot,token);
    if(!p || !p->executed || slot->reloc_state!=1u)
        return release_mount(slot,IFS_FS_INVALID);
    HfsMutation m={0};
    rc=mutation_load(&m,slot);
    if(rc!=IFS_FS_OK)goto done;
    for(uint32_t j=0u;j<p->count;++j) {
        const HrFile *f=&p->file[j];int found=0;
        for(uint32_t k=2u;k<m.count;++k) {
            const uint8_t *d=record_value(&m.records[k]);
            if(be32(d+20u)!=f->cnid)continue;
            if(be32(d+26u)!=f->size || be16(d+74u)!=f->target ||
               be16(d+76u)!=f->count || be16(d+80u) || be16(d+84u)){
                rc=IFS_FS_CORRUPT;goto done;
            }
            found=1;break;
        }
        if(!found){rc=IFS_FS_CORRUPT;goto done;}
        for(uint32_t k=0u;k<f->reserve;++k)
            if(used_block(m.bitmap,f->target+f->count+k)){
                rc=IFS_FS_CORRUPT;goto done;
            }
    }
done:
    mutation_free(&m);return release_mount(slot,rc);
}
static int hr_recover(void *volume,uint32_t flags,uint64_t *repaired)
{
    if(!repaired)return IFS_FS_INVALID;
    *repaired=0u;
    if(flags)return IFS_FS_INVALID;
    HfsMount *slot;int rc=acquire_mount(volume,&slot);
    if(rc!=IFS_FS_OK)return rc;
    if(!slot->writable)return release_mount(slot,IFS_FS_ACCESS);
    if(slot->reloc_plan)return release_mount(slot,IFS_FS_BUSY);
    if(slot->mutation_fault)return release_mount(slot,IFS_FS_IO_ERROR);
    HrGeometry g;HrFile f;int state=0;
    rc=hr_journal_read(&slot->host,&g,&f,&state);
    if(rc!=IFS_FS_OK)return release_mount(slot,rc);
    if(!state)return release_mount(slot,IFS_FS_UNSUPPORTED);
    if(state==2) {
        rc=hr_replay(&slot->host,&g,&f);
        if(rc!=IFS_FS_OK)return release_mount(slot,rc);
        ++slot->mount_recovered;
    }
    rc=hr_refresh(slot);
    if(rc!=IFS_FS_OK)return release_mount(slot,rc);
    slot->reloc_state=1u;*repaired=slot->mount_recovered;slot->mount_recovered=0u;
    return release_mount(slot,IFS_FS_OK);
}
static void hr_release(void *volume,void *token)
{
    HfsMount *slot;
    if(acquire_mount(volume,&slot)!=IFS_FS_OK)return;
    if(hr_get(slot,token))hr_drop_plan(slot);
    (void)release_mount(slot,IFS_FS_OK);
}

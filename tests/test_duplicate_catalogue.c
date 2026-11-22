/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common V2 host I/O feeding the exact HFS catalogue decoder compiled
 * into both hosted and freestanding providers. No private V1 API.
 */
#include "../native/hfs_catalog_v2.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){ \
    fprintf(stderr,"HFS catalogue key regression line %u: %s\n", \
            (unsigned)__LINE__,#x);return 1; \
}}while(0)
typedef struct {uint8_t media[4096];unsigned allocations,releases;} Fixture;
static int read_at(void *context,uint64_t offset,void *out,uint32_t n)
{
    Fixture *f=context;
    if(!n || offset%512u || n%512u || offset>sizeof(f->media) ||
       n>sizeof(f->media)-offset)return -1;
    memcpy(out,f->media+(size_t)offset,n);return 0;
}
static void *allocate(void *context,size_t n)
{
    void *p=malloc(n);
    if(p)++((Fixture *)context)->allocations;
    return p;
}
static void release(void *context,void *p)
{
    if(p)++((Fixture *)context)->releases;
    free(p);
}
static void big16(uint8_t *p,uint16_t n)
{p[0]=(uint8_t)(n>>8u);p[1]=(uint8_t)n;}
static void big32(uint8_t *p,uint32_t n)
{
    p[0]=(uint8_t)(n>>24u);p[1]=(uint8_t)(n>>16u);
    p[2]=(uint8_t)(n>>8u);p[3]=(uint8_t)n;
}
static void hfs_directory(uint8_t *p,uint32_t id,char name)
{
    p[0]=7u;big32(p+2u,2u);p[6]=1u;p[7]=(uint8_t)name;
    uint8_t *data=p+8u;
    data[0]=1u;big32(data+6u,id);
}
int main(void)
{
    Fixture f={0};
    IfsFsHostRuntimeV2 h={
        .abi_version=IFS_FS_HOST_ABI_V2,
        .structure_bytes=sizeof(h),
        .capabilities=IFS_FS_HOST_READ_ONLY,
        .volume_bytes=sizeof(f.media),
        .logical_sector_bytes=512u,.physical_sector_bytes=512u,
        .maximum_transfer_bytes=512u,.context=&f,
        .read_at=read_at,.allocate=allocate,.release=release
    };
    HfsCatalogueV2 v={0};
    v.host=&h;v.allocation_start=512u;v.allocation_bytes=512u;
    v.allocation_blocks=7u;v.catalogue_size=1024u;
    v.catalogue_extents[0].block_start=1u;
    v.catalogue_extents[0].block_count=2u;
    v.node_bytes=512u;v.node_count=2u;v.first_leaf=1u;
    uint8_t *leaf=f.media+1536u;
    leaf[8]=0xffu;big16(leaf+10u,1u);
    big16(leaf+510u,14u);big16(leaf+508u,114u);
    hfs_directory(leaf+14u,20u,'x');
    Scan exact={.parent=2u,.name="x",.name_length=1u};
    CHECK(catalogue_scan(&v,&exact)==IFS_FS_OK);
    CHECK(exact.found && exact.result.id==20u);
    CHECK(f.allocations==f.releases);

    /* The duplicate need not have identical case: HFS catalogue keys
     * are case-insensitive for native pathname lookup. */
    big16(leaf+10u,2u);
    big16(leaf+506u,214u);
    hfs_directory(leaf+114u,21u,'X');
    exact=(Scan){.parent=2u,.name="x",.name_length=1u};
    CHECK(catalogue_scan(&v,&exact)==IFS_FS_CORRUPT);
    CHECK(f.allocations==f.releases);

    Scan absent={.parent=2u,.name="y",.name_length=1u};
    CHECK(catalogue_scan(&v,&absent)==IFS_FS_NOT_FOUND);
    CHECK(f.allocations==f.releases);
    return 0;
}

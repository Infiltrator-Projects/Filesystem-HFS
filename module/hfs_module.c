/* SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone classic HFS provider. One public Common V2 ABI, no V1 loader.
 * The existing HFS native parser, journal and writer remain source-owned.
 * This descriptor publishes only independently qualified read-only probing;
 * mutation/defrag is not a V2 callback until ported and tested.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>

static uint16_t be16(const unsigned char *p)
{
    return ((uint16_t)p[0]<<8)|(uint16_t)p[1];
}
static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
/* Classic HFS MDB at absolute partition-relative byte 1024.
 * HFS+ wrappers cannot be mistaken for a writable classic-HFS volume. */
static int probe(const IfsFsHostRuntimeV2 *host,uint32_t *variant)
{
    unsigned char mdb[512];
    uint64_t start,allocation,physical,end;
    uint32_t block_bytes;
    uint16_t blocks,bitmap,first,free_blocks;
    if(!variant || !ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    *variant=0u;
    if(host->volume_bytes<1536u || host->maximum_transfer_bytes<sizeof(mdb))
        return IFS_FS_NOT_FOUND;
    if(host->read_at(host->context,1024u,mdb,sizeof(mdb))!=0)
        return IFS_FS_IO_ERROR;
    if(be16(mdb)!=UINT16_C(0x4244))
        return IFS_FS_NOT_FOUND;
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
    start=(uint64_t)bitmap*512u;
    allocation=(uint64_t)first*512u;
    physical=(uint64_t)blocks*block_bytes;
    if(start<1536u || start>=allocation || allocation>host->volume_bytes ||
       physical>host->volume_bytes-allocation)
        return IFS_FS_CORRUPT;
    end=start+((uint64_t)blocks+7u)/8u;
    if(end>allocation)
        return IFS_FS_CORRUPT; /* allocation bitmap overlaps data */
    *variant=1u; /* classic HFS, never HFS+ */
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
             IFS_FS_GROUP_CORE,0u,IFS_FS_CORE_PROBE},
    .probe=probe
};
static const IfsFsGroupHeaderV2 *query(
    uint32_t group_id,uint32_t minimum,uint32_t maximum)
{
    return group_id==IFS_FS_GROUP_CORE &&
           minimum<=IFS_FS_GROUP_ABI_V2 &&
           maximum>=IFS_FS_GROUP_ABI_V2 ? &core.header : NULL;
}
__attribute__((used,section(".ifsmeta")))
static const char identity[]="IFS-MODULE:hfs:common2";
static const IfsFsModuleApiV2 api={
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="hfs",
    .runtime_profiles=IFS_FS_PROFILE_HOSTED,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) {return &api;}

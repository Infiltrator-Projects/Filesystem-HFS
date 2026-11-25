/* SPDX-License-Identifier: GPL-3.0-or-later
 * Classic HFS catalogue reader, driven exclusively by Common V2 host I/O.
 * The format-owned decoder is shared by hosted and freestanding builds.
 */
#ifndef IFS_HFS_CATALOG_V2_H
#define IFS_HFS_CATALOG_V2_H
#include <stdint.h>
#include <infiltratr/filesystem_module_v2.h>
typedef struct {
    uint32_t block_start;
    uint32_t block_count;
} HfsExtentV2;
typedef struct {
    const IfsFsHostRuntimeV2 *host;
    uint64_t allocation_start;
    uint32_t allocation_bytes;
    uint32_t allocation_blocks;
    uint32_t catalogue_size;
    HfsExtentV2 catalogue_extents[3];
    uint32_t node_bytes, node_count, first_leaf;
    uint8_t *allocation_bitmap;
    uint32_t bitmap_bytes;
    uint8_t label[27],label_length;
} HfsCatalogueV2;
int hfs_catalogue_open_v2(const IfsFsHostRuntimeV2 *, HfsCatalogueV2 *);
void hfs_catalogue_close_v2(HfsCatalogueV2 *);
int hfs_catalogue_stat_v2(const HfsCatalogueV2 *, const char *, IfsFsEntryV2 *);
int hfs_catalogue_list_v2(const HfsCatalogueV2 *, const char *, uint32_t, IfsFsEntryV2 *);
int hfs_catalogue_read_v2(const HfsCatalogueV2 *, const char *, uint64_t, void *, uint32_t);
int hfs_catalogue_map_v2(const HfsCatalogueV2 *, const char *, uint64_t,
                         IfsFsExtentV2 *, uint32_t, uint32_t *);
#endif

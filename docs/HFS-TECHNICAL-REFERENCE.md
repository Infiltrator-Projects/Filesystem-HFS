# HFS: technical reference

## Historical scope and volume selection

Apple introduced Hierarchical File System in 1985 to replace the flat Macintosh File System. Classic HFS uses big-endian fields, 16-bit allocation-block identifiers, a separate volume bitmap, catalog and extents-overflow B-trees, and two forks per file. This is not HFS Plus. On partitioned media identify the actual Apple partition boundaries before interpreting volume-relative addresses. The Master Directory Block (MDB) is at byte 1024, signature 0x4244. An alternate MDB normally occupies the penultimate 512-byte logical sector. MFS magic 0xD2D7 and HFS Plus signatures represent other formats. Compare plausible geometry and metadata trees before accepting either MDB.

HFS arrived with the Macintosh HD20 and Finder 5.0 in 1985. A hierarchical catalog replaced MFS's increasingly inefficient flat-directory search on larger disks. Dual forks and original Macintosh collation are integral to compatibility.

## Master Directory Block: field layout

MDB offsets in bytes: signature 0, creation time 2, modification time 6, attributes 10, root file count 12, bitmap start 14, allocation search hint 16, allocation block count 18, allocation block bytes 20, clump size 24, first allocation sector 28, next CNID 30, free blocks 34, Pascal volume name 36 (28 bytes), backup date 64, backup sequence 68, write count 70, extents/catalog clump sizes 74/78, root directory count 82, file/folder counts 84/88, Finder info 92 (32 bytes). Embedded volume signature 124 and extent 126; extents tree size 130 and first three extents 134; catalog tree size 146 and first three extents 150. Check all declared spans, names, counters and version-specific fields.

## Allocation and two address units

The volume bitmap uses 512-byte sector addresses; allocation blocks begin at drAlBlSt*512, each drAlBlkSiz bytes. A set bitmap bit means allocated; bit numbering is MSB first. Allocation block n maps to drAlBlSt*512+n*drAlBlkSiz. For first allocation sector 32, allocation block bytes 4096 and allocation block 100, byte position is 425984. The bitmap for block 100 uses byte 12, mask 0x08. Exclude nonallocation sectors and bitmap padding. A 16-bit allocation count limits the number of allocation blocks regardless of device capacity; larger volumes therefore require larger allocation units. Do not confuse physical sectors, allocation blocks and B-tree node sizes.

## Forks and overflow mapping

An extent descriptor has 16-bit allocation-block start and count; a record holds three descriptors (12 bytes). A catalog file carries initial data-fork and resource-fork extents separately. Sum counts to locate a file logical allocation block; subsequent records are in the extents tree keyed by fork type (0 data, 0xff resource), 32-bit CNID and 16-bit first logical allocation-block index. The key has byte length at 0, fork type 1, CNID 2, first block 6. For extents (100,3),(300,2), logical block 4 maps to allocation block 301. Bound by logical EOF, not physical EOF. Verify continuation key and coverage, detect loops and reject overlap except native reservations. The extents tree's bootstrap cannot depend recursively on an unavailable overflow record.

## B-tree node geometry

Both trees are files, not fixed disk arrays. Classic HFS nodes are normally 512 bytes. Node descriptor contains forward and backward node numbers at 0/4, signed type at 8, height 9, record count 10 and reserved 12; records start after the 14-byte descriptor. The offset table grows backward from node end, with record 0 offset in the last two bytes and an extra free-space offset after the last record index. Header node 0 carries tree depth, root node, leaf count, first and last leaf, node size, maximum key length, total and free node counts. Validate bounds, node allocation map and ascending record offsets. Index records contain a key and child node number; leaf records contain key and value. Traverse by native collation and validate depth, links and cycles.

## Catalog keys and file record inventory

Catalog key: one-byte key length, reserved byte, parent CNID at 2, Pascal name at 6. Names are at most 31 bytes and compared with original Macintosh collation; do not apply arbitrary UTF-8 casefold. Values have type byte 1 directory, 2 file, 3 directory thread, 4 file thread. File value: flags at 2, Finder user info 4..19, CNID 20, obsolete start 24, data logical/physical EOF 26/30, resource start 34, resource logical/physical EOF 36/40, times 44/48/52, Finder info 56, clump 72, data extents 74, resource extents 86, reserved 98. Folder record contains valence at 4 and CNID at 6. Thread record contains parent at 10 and name at 14. Check CNID identity and thread relationships rather than treating a catalog key as file storage.

## Namespace mutation and preservation

Root CNID is 2, parent-of-root 1, extents tree 3, catalog tree 4 and bad-block file 5. New objects need unique catalog identifiers, a catalog record, applicable thread and parent valence update; fork extension allocates bitmap bits and updates its extent record or overflow tree. B-tree splits may require new nodes and changes to header and map records. Preserve resource forks, Finder type/creator information, dates and original character encoding. Native timestamps are based on the 1904 Macintosh epoch; classic HFS local-time conventions differ from HFS Plus UTC semantics. A file-copy implementation that discards the resource fork is not a complete compatible copier.

## Implementation and qualification boundary

Historical source 647fefce69ef reserved a mount session before initial media inspection; hosted probe, namespace read and format were declared, with separate freestanding qualification. Catalog and extent traversal must reject out-of-range records; a valid formatter or reader does not establish general writable namespace or physical firmware support.

Common V2 is the independent provider interface: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate real operation masks and media profiles. Use bounded host I/O, exclusive unmounted ownership and durable flush for mutation; distinguish unsupported, corrupt, I/O and stale-handle results. Test Media and Defragmenter use the same engine. Hosted, freestanding, BOOT, kernel, recovery and package qualification are independent. Current capability and release status belongs to the repository README and exact-source qualification records; historical evidence is not a current status claim.

Historical source or test reference: https://github.com/Infiltrator-Projects/Filesystem-HFS/tree/647fefce69ef .

## Consistency, relocation and sources

Classic HFS has no general metadata journal. Validate catalog and extent tree allocation, all forks and bad-block reservations against the bitmap; reconcile CNIDs, thread references and counts. To relocate a fork, reserve a destination, copy and verify bytes, update extent metadata, then retire old allocation after publication is durable. A crash can require a full repair; no atomic guarantee is implied. Test independently formatted classic Macintosh media with both forks, catalog splits, long native names, overflow extents and corrupt offset tables. Primary reference: Inside Macintosh: Files, Apple Computer. Pinned cross-check: https://github.com/torvalds/linux/tree/v6.12/fs/hfs (hfs.h, btree.h, extent.c, catalog.c, bitmap.c). Exact legacy collation, wrapper variants and repair ordering need independent fixtures.

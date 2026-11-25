# Classic HFS Design and Ownership

## Current production architecture

The independent `HFS.so` is the canonical installed filesystem provider. The hosted module declares probe, mount, unmount, stat, directory enumeration, read and format. Mount reservation precedes initial media inspection. Hosted and freestanding reader qualifications are separate. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

The hosted and freestanding profiles are qualified separately. Only the shared object and its technical PDF are installed by the filesystem package; the native RUN downloads its exact source revision. No separate per-format kernel or EFI driver or FUSE substitute is a production delivery path. See the [current README](../../../README.md) and [migration evidence](../../../MIGRATION_STATUS.md) for exact scope and qualification. The inherited design and source-ownership record below is historical where it describes older source layouts, implementation status or adapters; it remains valuable for format semantics and attribution.

## Historical design and source record

> **Working source (23 October 2026).** This filesystem's `linux/` directory contains the actual copied Linux implementation source, preserved with its original attribution and available for direct modification. It is not a replaceable research-only reference tree. The old independently-rewrite-first policy and any old directory-layout/refresh language below are historical, not a restriction on editing this working baseline. Only explicitly qualified drivers appear in `data/native-filesystems.tsv`.


## Classification

HFS is the classic Macintosh Hierarchical File System. It is a conventional
local block filesystem and therefore belongs in the canonical-engine
architecture.

## Historical state

HFS was originally documented as **imported-source development state**. The pinned upstream Linux HFS
source under `linux/` is preserved as compatibility evidence and is
not project-authored Filesystems code.

## Target layout

```text
native/filesystems/hfs/
  DESIGN.md
  core/                 canonical HFS format/filesystem semantics
  linux/                thin Linux VFS/module adapter
  windows/              thin Windows IFS/WDK adapter when implemented
  linux/      pinned upstream Linux evidence
```

## Canonical ownership

The future core owns HFS master-directory-block and volume geometry, allocation
bitmap, extents overflow and catalog B-tree semantics, file/resource forks,
directory/catalog records, classic Mac metadata/attributes, name translation
policy and exact mutation/recovery invariants.

Linux VFS objects, page/buffer APIs, mount/module lifetime and native permission
presentation remain adapter concerns.

## File-boundary rule

Upstream `btree.c`, `catalog.c`, `extent.c`, `inode.c`, `super.c` and
related files retain their original names only in `linux/`.
Project-owned source is cut by portable HFS responsibility, not copied Linux
translation units.

## Promotion sequence

Start read-only with bounded MDB, allocation, catalog and extent parsing against
independently created HFS images and malformed fixtures. Add writes only after
catalog/extents/allocation publication and recovery/verification rules are
explicit and destructively qualified.

## Failure policy

Invalid allocation/catalog/extents B-tree nodes, contradictory volume
geometry, out-of-range extents, malformed names and unsupported HFS states fail
closed.

## Copied implementation provenance

The editable `linux/` source contains original implementation code copied from the pinned Linux v6.12.107 source baseline. Original copyright, licence and attribution remain part of that code. Source refresh is not automatic and must never replace Filesystems modifications. Promotion into a managed native module requires its own host-integration and runtime qualification.

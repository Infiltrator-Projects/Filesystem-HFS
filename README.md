# Filesystem-HFS

## 1. Purpose and format scope

Classic Macintosh Hierarchical File System, not HFS Plus. This repository owns the format-specific implementation of `HFS.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

Master Directory Block, allocation bitmap, extents overflow and catalog B-tree govern volume and namespace geometry. Preserve classic catalog naming and allocation block semantics.

## 3. Common V2 implementation and capabilities

The hosted module declares probe, mount, unmount, stat, directory enumeration, read and format. Mount reservation precedes initial media inspection. Hosted and freestanding reader qualifications are separate. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

Catalog and extent traversal must reject out-of-range records. Do not claim general namespace mutation, HFS Plus support, or physical BOOT acceptance from reader and format tests. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/HFS.so` and `/System/Resources/Documentation/Filesystem-HFS.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-HFS/actions/runs/45526908856) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-HFS/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/hfs_module.c)
- [Native design or source provenance](native/filesystems/hfs/DESIGN.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/HFS.pdfstreams) and [PDF](docs/HFS.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.

The [engineering reference](docs/HFS-TECHNICAL-REFERENCE.md) adds field-level original-format guidance. The PDF and editable reference contain one integrated manual.

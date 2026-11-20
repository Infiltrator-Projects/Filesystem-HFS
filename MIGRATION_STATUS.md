# Classic HFS Common V2 migration and firmware qualification

The installed `/System/Filesystems/HFS.so` exports only Common V2. The
same format-owned `native/hfs_catalog_v2.c` implements both the hosted
and no-import freestanding UEFI reader, embedded byte-for-byte in the
canonical shared object's `.infiltrator_boot` section. No V1 runtime
adapter, Linux VFS driver, FUSE or filesystem-specific `.ko` is used.

Implemented read-only operations include real HFS MDB/bitmap probing,
independent sessions and stale-handle refusal, checked catalogue B-tree leaf
traversal with genuine HFS key padding, recursive ASCII path resolution,
directory enumeration, STAT, and data-fork READ through the source-owned
native HFS allocation extents. A per-mount verified MSB-first allocation
bitmap avoids repeated firmware reads. Host reads remain partition-relative,
and bounds, corrupt records, unallocated extents and missing files fail with
appropriate errors. No writes are advertised.

An external hfsutils formatter and copier build disposable real HFS volumes
with binary file contents, verified through the public Common V2 interface.
Tests cover valid media, corrupt MDB and catalog records, source-image
nonmutation and BOOT's unmodified 0.1.77 validator against both installed
DEB and CPU-native RUN modules. OVMF boots a genuine EFI-stub Linux kernel
and initramfs via HFS.so, checks loaded bytes and a guest marker, and refuses
corrupted source media. These checks must pass on the exact main commit before
an immutable release is published.

Known unsupported classic-HFS features are non-ASCII MacRoman path
conversion/case folding and file/catalogue extent-overflow records. These
return UNSUPPORTED rather than missing-file false success. Historical HFS
worker/fixtures remain only in non-installed regression targets.

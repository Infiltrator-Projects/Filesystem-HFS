# HFS source consolidation — 6 November 2026

The independent HFS repository now retains all 22 native source/design files from `Infiltrator-Projects/Filesystems` main commit `a253fb724ce1fd760a361219281effb6abd71367`, including the original kernel VFS driver and its provenance. The existing HFS analysis/defragmenter shared-object implementation was preserved, not replaced with a kernel module or declared to have untested writer functionality.

The `standalone/kernel.json` now builds `hfs.ko` from this repository's local Linux source, without downloading a historical Filesystems snapshot. Keep the `.so` build, kernel compilation and independent HFS media tests as separate qualification gates; remove the central engine only after build/CI consumer cutover is validated.

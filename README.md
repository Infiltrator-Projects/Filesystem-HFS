# Filesystem-HFS

The production provider is installed as `/System/Filesystems/HFS.so`
and its manual as
`/System/Resources/Documentation/Filesystem-HFS.pdf`.

Common V2 currently implements the real hfs on-media superblock/geometry
probe, strict read-only metadata mount session and volume-information query.
**It is NOT a BOOT-qualified filesystem reader.** Real Common V2 pathname
traversal, directory enumeration, file reads, native metadata/extent engines
and freestanding UEFI profile remain unimplemented.

The original native source, writer, worker, fixture and Linux implementation
remain in the repository. Legacy callbacks are built solely into a
non-installed regression library, and no filesystem-specific Linux `.ko`
is distributed. The tested portable `.deb` and source-embedded native
`.run` remain CI artifacts; firmware publication is deliberately withheld
until the production validator, corrupt-media checks and OVMF handoff pass.

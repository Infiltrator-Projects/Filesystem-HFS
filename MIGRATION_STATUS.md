# HFS Common V2 migration status

- Hosted V2 probe: implemented; real on-media header/geometry validation
- Hosted V2 metadata-only read-only mount and volume query: implemented
- V2 path resolution, STAT, LIST and READ: **not implemented**
- Shared hosted/freestanding native reader: **not implemented**
- Packaged firmware ELF accepted by BOOT validator: **not qualified**
- OVMF real kernel/initramfs reads: **not qualified**
- Legacy workers/fixtures: retained for **non-installed tests only**
- Production package: one Common V2 shared object and one PDF, no .ko
- Publication: held until full BOOT requirements pass

A metadata-only V2 mount is not evidence of a working filesystem namespace.

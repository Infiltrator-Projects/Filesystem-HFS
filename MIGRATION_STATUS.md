# Classic HFS driver migration

The first-party classic HFS analysis, Defrag, Growth Defrag and durable recovery
writer is now built in HFS.so. The Test Media HFS layout creator and verifier
were transferred into the same module with a typed callback to the host-owned
generic corpus and raw I/O. This remains separate from HFS+/HFSX.

**Not complete** until Defragmenter removes the native HFS worker and Test Media
writer, invokes HFS.so exclusively and passes full end-to-end regression.

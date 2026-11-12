---
synopsis: "The garbage collector deletes garbage in parallel"
issues: 9581
---

The garbage collector now deletes dead store paths in background threads, while it continues looking for more garbage.
Each dead store path is first atomically moved into a temporary trash directory inside the Nix store, so a concurrent build or substitution of the same path is unaffected.
Deleting several store paths concurrently can make garbage collection faster on storage with a high latency per operation, such as network block storage.

The number of threads is controlled by the new [`gc-delete-threads`](@docroot@/command-ref/conf-file.md#conf-gc-delete-threads) setting, which defaults to 4.
Raise it on dedicated local NVMe storage or uncontended high-IOPS volumes; on shared or IOPS-capped volumes more threads mostly add I/O pressure on other users of the device.
Setting it to `1` restores the previous behaviour of deleting store paths in place, one at a time.
If a garbage collection is interrupted, the next one deletes the trash it left behind.

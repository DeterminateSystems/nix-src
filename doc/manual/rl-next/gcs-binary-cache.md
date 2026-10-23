---
synopsis: Native Google Cloud Storage binary caches
---

Nix can read and write Google Cloud Storage binary caches using `gs://bucket/prefix` or `gcs://bucket/prefix`.
The Google Cloud C++ client supplies Application Default Credentials, including workload identity federation, metadata-server credentials, service-account impersonation, and token refresh.
Uploads use resumable transfers without buffering entire NARs in memory.

For daemon substitutions, configure credentials in the daemon's environment or use the VM's attached service account.
See the [GCS store documentation](@docroot@/store/types/gcs-binary-cache-store.md) for details.

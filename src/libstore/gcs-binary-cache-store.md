R"(

**Store URL format**: `gs://`*bucket-name*`/`*optional-prefix* (also `gcs://`)

This store reads and writes the standard [binary cache](@docroot@/protocols/binary-cache/index.md) layout in Google Cloud Storage.
It requires a build of Nix with Google Cloud Storage support (`-Dgcs=enabled` in the `nix-store` Meson project).
The Nix package enables this support by default on supported, dynamically linked platforms when Nix and the Google client use the same C++ standard library.

### Authentication

Nix uses the Google Cloud C++ Storage client's [Application Default Credentials](https://cloud.google.com/docs/authentication/application-default-credentials) (ADC).
The client discovers credentials from `GOOGLE_APPLICATION_CREDENTIALS`, the local ADC file, or the metadata server for an attached service account.
It supports external-account credentials for workload identity federation, service-account impersonation, and automatic access-token refresh.
No GCS HMAC key or proxy is required.

On a GCE VM, grant the attached service account access to the bucket.
For workload identity federation, set `GOOGLE_APPLICATION_CREDENTIALS` to the credential configuration file provided by your CI environment.
For local development, `gcloud auth application-default login` creates an ADC file.

Readers need `storage.objects.get`.
Writers also need `storage.objects.create` and `storage.objects.delete` to replace existing objects.
Nix does not create the bucket or require bucket listing permissions.
Authentication and permission errors are reported as errors, rather than treated as missing objects.

### Writing and reading a cache

```console
$ nix copy --to 'gs://example-nix-cache/team' ./result
$ nix copy --from 'gs://example-nix-cache/team' /nix/store/…
```

The optional prefix places all cache objects, including `nix-cache-info`, under that prefix.
Uploads are streamed using GCS resumable uploads, with bounded buffering.
Transient GCS request failures are retried up to three times; interruption stops further retries.
Uploads and downloads are checked by the client for transport and checksum errors.
Nix's normal NAR hashes and signature checks still apply.
Configure cache signing keys and `trusted-public-keys` as for other binary caches.

### Multi-user installations

Credentials belong to the process accessing GCS.
`nix copy --to gs://…` accesses the destination from the client process, even when the source store is a daemon.
Substitution during a build runs in `nix-daemon`, so ADC must be available to that daemon.

On GCE, the daemon can use the VM's attached service account directly.
For file-based credentials, configure `GOOGLE_APPLICATION_CREDENTIALS` in the daemon service's environment and ensure that the daemon can read both the ADC configuration and any referenced subject-token files.
Restart the daemon after changing its environment.
Exporting the variable in an interactive shell does not pass it to an already running daemon.
Nix does not forward client credentials to the daemon or accept a credential-file path in the store URL.

Configure the daemon's cache in `nix.conf` (or `nix.custom.conf` for Determinate Nix), for example:

```ini
extra-substituters = gs://example-nix-cache/team
extra-trusted-public-keys = example-nix-cache-1:…
```

)"

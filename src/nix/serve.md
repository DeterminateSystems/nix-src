R""(

# Examples

* Serve the local Nix store on the default port (8080), listening on localhost only:

  ```console
  # nix serve
  Listening on http://127.0.0.1:8080/
  ```

* Serve the local store on all interfaces, on port 9000:

  ```console
  # nix serve --listen-address 0.0.0.0 --port 9000
  ```

  On another machine, you can then use this server as a substituter:

  ```console
  # nix copy --from http://other-host:8080 /nix/store/...-hello-2.12.1
  ```

* Serve a chroot store as a binary cache:

  ```console
  # nix serve --store /tmp/my-store
  ```

# Description

`nix serve` runs an HTTP server that exposes a Nix store as a [binary
cache](@docroot@/glossary.md#gloss-binary-cache). Clients can fetch
store paths from this server by adding its URL to their list of
[substituters](@docroot@/command-ref/conf-file.md#conf-substituters).

NARs are served uncompressed.

> **Note**
>
> `nix serve` only handles `GET` and `HEAD` requests; it cannot be
> used to upload paths to the store. It also does not implement
> authentication or TLS — if you want to expose it to the public
> internet, run it behind a reverse proxy such as nginx.

To shut down the server, send it a `SIGINT` signal.

)""

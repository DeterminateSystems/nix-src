# Lockable HTTP Protocol

Nix can fetch a flake input over HTTP, or from the file system for `file://` URLs.
If the server does not support the Lockable HTTP Protocol, the user must make sure that the URL always gives the same contents.

An HTTP server can return an "immutable" URL for the lock file.
Then the user can put an input in `flake.nix` that asks for the most recent version of a resource (for example, `https://example.org/hello/latest.tar.gz`), while `flake.lock` records a URL whose contents do not change (for example, `https://example.org/hello/<revision>.tar.gz`).
To do this, the server must send an [HTTP `Link` header](https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Link) with the `rel` attribute set to `immutable`, as follows:

```
Link: <flakeref>; rel="immutable"
```

(The `<` and `>` characters around *flakeref* are necessary.)

*flakeref* must have the same input type as the input that Nix fetched.
Nix gives an error if the two types are different.
For the two input types that this protocol applies to, refer to [Tarball inputs](#tarball-inputs) and [File inputs](#file-inputs).

*flakeref* can contain the flake attributes `narHash`, `rev`, `revCount` and `lastModified`.
If *flakeref* contains `narHash`, its value must be the [NAR hash][Nix Archive] of the contents, as computed by `nix hash path`.
Nix compares the contents that it downloaded against `narHash`, and gives an error if the two values are different.
Nix does not check `rev` and `revCount`.
These two attributes are useful when the input is a mirror of a fetcher type that has them, such as Git or GitHub.

```
Link: <https://example.org/hello/442793d9ec0584f6a6e82fa253850c8085bb150a.tar.gz
  ?rev=442793d9ec0584f6a6e82fa253850c8085bb150a
  &revCount=835
  &narHash=sha256-GUm8Uh/U74zFCwkvt9Mri4DSM%2BmHj3tYhXUkYpiv31M%3D>; rel="immutable"
```

(The line breaks in this example make it easy to read. Do not put them in the actual response.)

## Tarball inputs

Nix unpacks a tarball input into a tree.
Nix uses this input type when the URL has a tarball extension, such as `.tar.gz` or `.zip`, or when the input is a flake.

For a tarball input, *flakeref* must be a tarball flakeref.

The value of the `lastModified` flake attribute is the timestamp of the most recent file in the tarball.

## File inputs

Nix puts a file input into the store as a single file.
Nix uses this input type when the URL has no tarball extension and the input has `flake = false`.
The URL does not have to look like a file, and it can contain query parameters.

> **Example**
>
> ```nix
> # flake.nix
> {
>   inputs.determinate-pkg = {
>     url = "https://install.determinate.systems/determinate-pkg/stable/Universal";
>     flake = false;
>   };
>   outputs = { foo }: { /* ... */ };
> }
> ```

For a file input, *flakeref* must be a file flakeref.

```
Link: <https://install.determinate.systems/determinate-pkg/tag/v3.22.0/Universal>; rel="immutable"
```

The `lastModified` attribute does not apply to file inputs.

## Gitea and Forgejo support

This protocol is supported by Gitea since v1.22.1 and by Forgejo since v7.0.4/v8.0.0 and can be used with the following flake URL schema:

```
https://<domain name>/<owner>/<repo>/archive/<reference or revision>.tar.gz
```

> **Example**
>
>
> ```nix
> # flake.nix
> {
>    inputs = {
>      foo.url = "https://gitea.example.org/some-person/some-flake/archive/main.tar.gz";
>      bar.url = "https://gitea.example.org/some-other-person/other-flake/archive/442793d9ec0584f6a6e82fa253850c8085bb150a.tar.gz";
>      qux = {
>        url = "https://forgejo.example.org/another-person/some-non-flake-repo/archive/development.tar.gz";
>        flake = false;
>      };
>    };
>    outputs = { foo, bar, qux }: { /* ... */ };
> }
```

[Nix Archive]: @docroot@/store/file-system-object/content-address.md#serial-nix-archive

{{#include provenance-v1-fixed.md}}

## Examples

### Build provenance

A store object built from a flake output, showing the full provenance chain back to the flake's source tree:

```json
{{#include schema/provenance-v1/build.json}}
```

### Copied provenance

A store object substituted from a binary cache:

```json
{{#include schema/provenance-v1/copied.json}}
```

### Source file

A file inside a Git repository:

```json
{{#include schema/provenance-v1/subpath.json}}
```

### Fetched URL

```json
{{#include schema/provenance-v1/fetchurl.json}}
```

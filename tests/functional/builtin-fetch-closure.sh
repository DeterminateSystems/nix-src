#!/usr/bin/env bash

source common.sh

enableFeatures "fetch-closure impure-derivations"

TODO_NixOS

clearStore
clearCacheCache

# Old daemons don't properly zero out the self-references when
# calculating the CA hashes, so this breaks `nix store
# make-content-addressed` which expects the client and the daemon to
# compute the same hash
requireDaemonNewerThan "2.16.0pre20230524"

# Initialize binary cache.
nonCaPath=$(nix build --json --file ./dependencies.nix --no-link | jq -r .[].outputs.out)
caPath=$(nix store make-content-addressed --json "$nonCaPath" | jq -r '.rewrites | map(.) | .[]')
nix copy --to file://"$cacheDir" "$nonCaPath"
nix copy --to file://"$cacheDir" "$caPath"

# Test basic builtin:fetch-closure with input-addressed path
clearStore

[ ! -e "$nonCaPath" ]

outPath=$(nix-build --no-out-link --expr "
  derivation {
    name = \"fetch-test\";
    builder = \"builtin:fetch-closure\";
    system = \"$system\";
    __impure = true;
    __structuredAttrs = true;
    fromStore = \"file://$cacheDir\";
    fromPath = \"$nonCaPath\";
    inputAddressed = true;
  }
")

echo "outPath = $outPath"

[ "$outPath" = "$nonCaPath" ]
[ -e "$nonCaPath" ]

clearStore

# Test builtin:fetch-closure with CA path
clearStore

[ ! -e "$caPath" ]

outPath=$(nix-build --no-out-link --expr "
  derivation {
    name = \"fetch-test-ca\";
    builder = \"builtin:fetch-closure\";
    system = \"$system\";
    __impure = true;
    __structuredAttrs = true;
    fromStore = \"file://$cacheDir\";
    fromPath = \"$caPath\";
  }
")

echo "outPath = $outPath"

[ "$outPath" = "$caPath" ]
[ -e "$caPath" ]

clearStore

# Test builtin:fetch-closure with full path
clearStore

[ ! -e "$nonCaPath" ]

outPath=$(nix-build --no-out-link --expr "
  derivation {
    name = \"fetch-test-fullpath\";
    builder = \"builtin:fetch-closure\";
    system = \"$system\";
    __impure = true;
    __structuredAttrs = true;
    fromStore = \"file://$cacheDir\";
    fromPath = \"/nix/store/$(basename $nonCaPath)\";
    inputAddressed = true;
  }
")

echo "outPath = $outPath"

[ "$outPath" = "$nonCaPath" ]
[ -e "$nonCaPath" ]

clearStore

# Test that missing __structuredAttrs fails
expectStderr 1 nix-build --no-out-link --expr "
  derivation {
    name = \"fetch-test-nostruct\";
    builder = \"builtin:fetch-closure\";
    system = \"$system\";
    __impure = true;
    fromStore = \"file://$cacheDir\";
    fromPath = \"$nonCaPath\";
    inputAddressed = true;
  }
"

# Test that missing __impure fails (derivation won't have predetermined path)
expectStderr 1 nix-build --no-out-link --expr "
  derivation {
    name = \"fetch-test-noimpure\";
    builder = \"builtin:fetch-closure\";
    system = \"$system\";
    __structuredAttrs = true;
    fromStore = \"file://$cacheDir\";
    fromPath = \"$nonCaPath\";
    inputAddressed = true;
  }
"

# Test that URL query parameters aren't allowed
expectStderr 100 nix-build --no-out-link --expr "
  derivation {
    name = \"fetch-test-query\";
    builder = \"builtin:fetch-closure\";
    system = \"$system\";
    __impure = true;
    __structuredAttrs = true;
    fromStore = \"file://$cacheDir?foo=bar\";
    fromPath = \"$nonCaPath\";
    inputAddressed = true;
  }
" | grepQuiet "does not support URL query parameters"

# Test CA/input-addressed mismatch detection
expectStderr 100 nix-build --no-out-link --expr "
  derivation {
    name = \"fetch-test-mismatch\";
    builder = \"builtin:fetch-closure\";
    system = \"$system\";
    __impure = true;
    __structuredAttrs = true;
    fromStore = \"file://$cacheDir\";
    fromPath = \"$caPath\";
    inputAddressed = true;
  }
" | grepQuiet "is content-addressed, but 'inputAddressed' is set to 'true'"

expectStderr 100 nix-build --no-out-link --expr "
  derivation {
    name = \"fetch-test-mismatch2\";
    builder = \"builtin:fetch-closure\";
    system = \"$system\";
    __impure = true;
    __structuredAttrs = true;
    fromStore = \"file://$cacheDir\";
    fromPath = \"$nonCaPath\";
  }
" | grepQuiet "is input-addressed, but 'inputAddressed' is not set"

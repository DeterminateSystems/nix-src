#!/usr/bin/env bash

# Test that a binary cache refuses to publish a .narinfo whose NarHash or
# NarSize doesn't match the NAR it was given.

source common.sh

TODO_NixOS

needLocalStore "corrupts store paths in place"

clearStore
clearBinaryCache

# Assert that nothing was written to the binary cache for a path.
assertNotUploaded() {
    local hashPart
    hashPart=$(basename "$1")
    hashPart=${hashPart%%-*}
    [[ ! -e "$cacheDir/$hashPart.narinfo" ]]
    [[ -z "$(find "$cacheDir" -name '*.nar*' -print -quit)" ]]
}

# NarHash mismatch: change the contents of a registered path without
# changing its size. `nix copy` goes through `addMultipleToStore()`.
echo "nar-verify-good" > "$TEST_ROOT/payload"
path=$(nix store add --mode flat "$TEST_ROOT/payload")
chmod u+w "$path"
echo "nar-verify-evil" > "$path"
expectStderr 1 nix copy --to "file://$cacheDir" "$path" \
    | grepQuiet "hash mismatch uploading path '$path'"
assertNotUploaded "$path"

# NarSize mismatch: make the registered path bigger.
echo "nar-verify-size" > "$TEST_ROOT/payload"
path=$(nix store add --mode flat "$TEST_ROOT/payload")
chmod u+w "$path"
echo "more" >> "$path"
expectStderr 1 nix copy --to "file://$cacheDir" "$path" \
    | grepQuiet "size mismatch uploading path '$path'"
assertNotUploaded "$path"

# The same check on `addToStore()`, via a nario whose NAR doesn't match the
# path info it carries.
echo "nar-verify-nario-good" > "$TEST_ROOT/payload"
path=$(nix store add --mode flat "$TEST_ROOT/payload")
nix nario export --format 2 "$path" > "$TEST_ROOT/good.nario"
sed 's/nar-verify-nario-good/nar-verify-nario-evil/' "$TEST_ROOT/good.nario" > "$TEST_ROOT/bad.nario"
expectStderr 1 nix nario import --no-check-sigs --store "file://$cacheDir" < "$TEST_ROOT/bad.nario" \
    | grepQuiet "hash mismatch uploading path '$path'"
assertNotUploaded "$path"

# Paths whose info matches their NAR are still uploaded, by both routes.
nix nario import --no-check-sigs --store "file://$cacheDir" < "$TEST_ROOT/good.nario"
nix path-info --store "file://$cacheDir" "$path"

echo "nar-verify-copy-good" > "$TEST_ROOT/payload"
path=$(nix store add --mode flat "$TEST_ROOT/payload")
nix copy --to "file://$cacheDir" "$path"
nix path-info --store "file://$cacheDir" "$path"

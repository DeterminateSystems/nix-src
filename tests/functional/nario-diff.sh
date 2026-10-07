#!/usr/bin/env bash

source common.sh

TODO_NixOS

clearStore

old=$(nix-build nario-diff.nix --argstr version 1 --no-out-link)
new=$(nix-build nario-diff.nix --argstr version 2 --no-out-link)
shared=$(nix-store -qR "$new" | grep nario-shared)
extra=$(nix-store -qR "$new" | grep nario-extra)

nix key generate-secret --key-name my-key > "$TEST_ROOT"/secret
public_key=$(nix key convert-secret-to-public < "$TEST_ROOT"/secret)
nix store sign --key-file "$TEST_ROOT/secret" -r "$new"

# Binary diffs require format 2.
expectStderr 1 nix nario export --format 1 -r "$new" --base "$old" | grepQuiet "only supported in nario version 2"

nix nario export --format 2 -r "$old" > "$TEST_ROOT"/old.nario
nix nario export --format 2 -r "$new" > "$TEST_ROOT"/full.nario
nix nario export --format 2 -r "$new" --base "$old" > "$TEST_ROOT"/diff.nario

# The diff should be much smaller than the full export.
(( $(stat -c %s "$TEST_ROOT"/diff.nario) * 4 < $(stat -c %s "$TEST_ROOT"/full.nario) ))

# Test `nix nario list`.
nix nario list < "$TEST_ROOT"/diff.nario | grepQuiet "^$new: [0-9]* bytes, diff against $old ([0-9]* bytes)$"
nix nario list < "$TEST_ROOT"/diff.nario | grepQuiet "^$shared: expected to be present$"
nix nario list < "$TEST_ROOT"/diff.nario | grepQuiet "^$extra: [0-9]* bytes$"
nix nario list -R < "$TEST_ROOT"/diff.nario | grepQuiet "^$extra/data$"

json=$(nix nario list --json < "$TEST_ROOT/diff.nario")
[[ $(printf "%s" "$json" | jq -r ".paths.\"$new\".diff.base") = "$old" ]]
[[ $(printf "%s" "$json" | jq -r ".paths.\"$new\".diff.baseNarHash") = $(nix path-info --json "$old" | jq -r ".[].narHash") ]]
[[ $(printf "%s" "$json" | jq -r ".paths.\"$shared\".present") = true ]]
[[ $(printf "%s" "$json" | jq -r ".paths.\"$extra\".diff") = null ]]

# Importing into an empty store fails because the base closure is missing.
clearStore
expectStderr 1 nix nario import --no-check-sigs < "$TEST_ROOT"/diff.nario | grepQuiet "nario requires path '$shared' to already be valid in the store"

# Importing into a store that lacks the diff base fails.
clearStore
nix nario import --no-check-sigs < "$TEST_ROOT"/old.nario
nix-store --delete "$old"
expectStderr 1 nix nario import --no-check-sigs < "$TEST_ROOT"/diff.nario | grepQuiet "binary diff against path '$old', which is not valid"

# Importing into a store that has the base closure succeeds, and preserves signatures.
clearStore
nix nario import --no-check-sigs < "$TEST_ROOT"/old.nario
expectStderr 1 nix nario import < "$TEST_ROOT"/diff.nario | grepQuiet "lacks a signature"
nix nario import --trusted-public-keys "$public_key" < "$TEST_ROOT"/diff.nario
[[ $(nix path-info --json "$new" | jq -r .[].signatures[]) =~ my-key: ]]
nix store verify --no-trust -r "$new"
grepQuiet "^2$" "$new/data"
[[ $(readlink "$new/extra") = "$extra" ]]

# Importing again is a no-op.
nix nario import --no-check-sigs < "$TEST_ROOT"/diff.nario

# Importing fails if the base has a different NAR hash.
clearStore
nix nario import --no-check-sigs < "$TEST_ROOT"/old.nario
chmod -R u+w "$old"
echo corrupt >> "$old/data"
expectStderr 1 nix nario import --no-check-sigs < "$TEST_ROOT"/diff.nario | grepQuiet "binary diff against path '$old', which has NAR hash '.*' instead of the expected"

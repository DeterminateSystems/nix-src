#!/usr/bin/env bash

source common.sh

TODO_NixOS

clearStore

outPath=$(nix-build dependencies.nix --no-out-link)
drvPath=$(nix path-info --json "$outPath" | jq -r .\""$outPath"\".deriver)

nix-store --export $outPath > $TEST_ROOT/exp
expectStderr 1 nix nario export "$outPath" | grepQuiet "required argument.*missing"
nix nario export --format 1 "$outPath" > $TEST_ROOT/exp2
cmp "$TEST_ROOT/exp" "$TEST_ROOT/exp2"

nix-store --export $(nix-store -qR $outPath) > $TEST_ROOT/exp_all

nix nario export --format 1 -r "$outPath" > $TEST_ROOT/exp_all2
cmp "$TEST_ROOT/exp_all" "$TEST_ROOT/exp_all2"

if nix-store --export $outPath >/dev/full ; then
    echo "exporting to a bad file descriptor should fail"
    exit 1
fi

clearStore

if nix-store --import < $TEST_ROOT/exp; then
    echo "importing a non-closure should fail"
    exit 1
fi

clearStore

nix-store --import < $TEST_ROOT/exp_all

nix-store --export $(nix-store -qR $outPath) > $TEST_ROOT/exp_all2

clearStore

# Regression test: the derivers in exp_all2 are empty, which shouldn't
# cause a failure.
nix-store --import < $TEST_ROOT/exp_all2

# Test `nix nario import` on files created by `nix-store --export`.
clearStore
expectStderr 1 nix nario import < $TEST_ROOT/exp_all | grepQuiet "lacks a signature"
nix nario import --no-check-sigs < $TEST_ROOT/exp_all
nix path-info "$outPath"

# Test `nix nario list`.
nix nario list < $TEST_ROOT/exp_all
nix nario list < $TEST_ROOT/exp_all | grepQuiet ".*dependencies-input-0.*bytes"

# Test format 2 (including signatures).
nix key generate-secret --key-name my-key > $TEST_ROOT/secret
public_key=$(nix key convert-secret-to-public < $TEST_ROOT/secret)
nix store sign --key-file "$TEST_ROOT/secret" -r "$outPath"
nix nario export --format 2 -r "$outPath" > $TEST_ROOT/exp_all
clearStore
expectStderr 1 nix nario import < $TEST_ROOT/exp_all | grepQuiet "lacks a signature"
nix nario import --trusted-public-keys "$public_key" < $TEST_ROOT/exp_all
[[ $(nix path-info --json "$outPath" | jq -r .[].signatures[]) =~ my-key: ]]

# Test json listing.
json=$(nix nario list --json < "$TEST_ROOT/exp_all")
[[ $(printf "%s" "$json" | jq -r ".paths.\"$outPath\".deriver") = "$drvPath" ]]
[[ $(printf "%s" "$json" | jq -r ".paths.\"$outPath\".contents.type") = directory ]]
[[ $(printf "%s" "$json" | jq -r ".paths.\"$outPath\".contents.entries.foobar.type") = regular ]]
[[ $(printf "%s" "$json" | jq ".paths.\"$outPath\".contents.entries.foobar.size") = 7 ]]

json=$(nix nario list --json --no-contents < "$TEST_ROOT/exp_all")
[[ $(printf "%s" "$json" | jq -r ".paths.\"$outPath\".deriver") = "$drvPath" ]]
[[ $(printf "%s" "$json" | jq -r ".paths.\"$outPath\".contents.type") = null ]]

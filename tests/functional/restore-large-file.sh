#!/usr/bin/env bash

# Substitute a file larger than several writeback chunks (32 MiB, fs-sink.cc)
# with fsync-store-paths on, and check that its contents survive and that the
# path is registered. Whether writeback is started isn't observable here.

source common.sh

TODO_NixOS

needLocalStore "'--no-check-sigs' can’t be used with the daemon"

clearStore
clearBinaryCache

head -c $((100 * 1024 * 1024 + 12345)) /dev/urandom > "$TEST_ROOT/large"
large=$(nix store add-file --store "file://$cacheDir?compression=none" "$TEST_ROOT/large")

nix copy --from "file://$cacheDir" --no-check-sigs --option fsync-store-paths true "$large"

nix path-info "$large"
nix store cat "$large" | cmp - "$TEST_ROOT/large"
nix store verify --no-trust "$large"

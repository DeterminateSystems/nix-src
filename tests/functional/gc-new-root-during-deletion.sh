#!/usr/bin/env bash

# Test that a path that becomes a temporary root after the garbage
# collector has decided to delete it is kept, along with its closure,
# while its referrers are still deleted.
source common.sh

TODO_NixOS

needLocalStore "the GC test needs a synchronisation point"

clearStore

cat > "$TEST_ROOT/chain.nix" <<EOF
with import ${config_nix};
rec {
  bottom = mkDerivation {
    name = "gc-chain-bottom";
    buildCommand = "mkdir \$out";
  };
  middle = mkDerivation {
    name = "gc-chain-middle";
    inherit bottom;
    buildCommand = "mkdir \$out; echo \$bottom > \$out/ref";
  };
  top = mkDerivation {
    name = "gc-chain-top";
    inherit middle;
    buildCommand = "mkdir \$out; echo \$middle > \$out/ref";
  };
}
EOF

bottom=$(nix-build "$TEST_ROOT/chain.nix" -A bottom --no-out-link)
middle=$(nix-build "$TEST_ROOT/chain.nix" -A middle --no-out-link)
top=$(nix-build "$TEST_ROOT/chain.nix" -A top --no-out-link)

# This FIFO is read when the garbage collector has found the paths to
# delete, so writing to it tells us that it got there.
fifo1=$TEST_ROOT/gc-reached.fifo
mkfifo "$fifo1"

# This FIFO is read just before the actual deletion.
fifo2=$TEST_ROOT/gc-continue.fifo
mkfifo "$fifo2"

# Deleting `bottom` and its referrers gives a single batch of paths
# that are deleted in the order `top`, `middle`, `bottom`.
_NIX_TEST_GC_SYNC_4=$fifo1 _NIX_TEST_GC_SYNC_5=$fifo2 \
    nix store delete --also-referrers "$bottom" 2> "$TEST_ROOT/gc.log" &
pid=$!

echo > "$fifo1"

# Make `middle` a temporary root by sending it to the garbage collector.
nix-store -r "$middle"

echo > "$fifo2"

# `bottom` couldn't be deleted, so this should fail.
if wait "$pid"; then
    fail "deleting a path that became alive should fail"
fi
grepQuiet "in use by another process" "$TEST_ROOT/gc.log"

# If the closure of `middle` isn't marked as alive, the garbage
# collector tries to delete `bottom`, which fails because it's still
# referenced, so check for that as well as the paths surviving.
grepQuietInverse "BUG:" "$TEST_ROOT/gc.log"

[[ ! -e "$top" ]] || fail "top should have been deleted"
[[ -e "$middle" ]] || fail "middle became a temporary root, it shouldn't have been deleted"
[[ -e "$bottom" ]] || fail "bottom is in the closure of a temporary root, it shouldn't have been deleted"
nix-store --check-validity "$middle" "$bottom"

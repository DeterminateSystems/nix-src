#!/usr/bin/env bash

# `nix-store --verify` must not unregister paths that are added while it
# runs. It scans the store directory and then queries the database, and
# the scan is not a snapshot: a path whose file appears after the scan
# went past its name, and which is registered before the query, is valid
# but missing from the scan.
source common.sh

TODO_NixOS

needLocalStore "the test needs a synchronisation point in the verifying process"

# The verify writes to this FIFO when it has scanned the store directory,
scanned=$TEST_ROOT/verify-scanned.fifo
# and then reads from this one before it queries the database.
resume=$TEST_ROOT/verify-resume.fifo
mkfifo "$scanned" "$resume"

_NIX_TEST_VERIFY_SYNC_1=$scanned _NIX_TEST_VERIFY_SYNC_2=$resume \
    nix-store --verify --check-contents --repair > "$TEST_ROOT/verify.log" 2>&1 &
pid=$!

cat "$scanned" > /dev/null

# Write two .drv files, one referring to the other, as an evaluation does.
drvs=$(nix-instantiate --expr "
  with import ${config_nix};
  rec {
    a = mkDerivation { name = \"verify-concurrent-a\"; buildCommand = \"echo a > \$out\"; };
    b = mkDerivation { name = \"verify-concurrent-b\"; buildCommand = \"echo \${a} > \$out\"; };
  }")

echo > "$resume"
wait "$pid"

cat "$TEST_ROOT/verify.log" >&2
(! grepQuiet disappeared "$TEST_ROOT/verify.log")
for drv in $drvs; do
    test -e "$drv"
    nix-store --check-validity "$drv"
done

# A path whose file is really gone is still removed from the database.
echo gone > "$TEST_ROOT/gone"
gone=$(nix-store --add "$TEST_ROOT/gone")
rm -f "$gone"
nix-store --verify > "$TEST_ROOT/verify-gone.log" 2>&1
grepQuiet "path '$gone' disappeared, removing from database" "$TEST_ROOT/verify-gone.log"
(! nix-store --check-validity "$gone")

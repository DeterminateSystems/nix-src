#!/usr/bin/env bash

# Test that the garbage collector doesn't block clients that register
# temporary roots while it marks the closure of a temporary root alive.

source common.sh

TODO_NixOS

needLocalStore "the GC test needs a synchronisation point"

clearStore

ready=$TEST_ROOT/ready
resume=$TEST_ROOT/resume
sync=$TEST_ROOT/sync
rm -f "$ready" "$resume" "$sync"
mkfifo "$ready" "$resume" "$sync"

# A build in progress, so its .drv is only rooted by a temporary root.
nix-build --no-out-link -E "
  derivation {
    name = \"gc-mark-alive-unlocked\";
    system = builtins.currentSystem;
    builder = \"$BASH\";
    args = [ \"-c\" \"echo > $ready; read -r _ < $resume; echo > \$out\" ];
  }" > /dev/null &
buildPid=$!
read -r _ < "$ready"

# Start a GC that stops when it marks the .drv alive. Opening the FIFO
# returns once the GC is waiting on it.
_NIX_TEST_GC_SYNC_3=$sync nix-store --gc &
gcPid=$!
exec {syncFd}>"$sync"

# Registering a temporary root must not wait for the GC.
status=0
timeout 20 nix-store --add ./simple.nix > /dev/null || status=$?

exec {syncFd}>&-
wait "$gcPid"
echo > "$resume"
wait "$buildPid"

[[ $status == 0 ]] || fail "registering a temporary root blocked while the GC marked a closure alive"

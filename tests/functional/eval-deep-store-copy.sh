#!/usr/bin/env bash

source common.sh

# Copying a source to the store (which switches to a coroutine) while the
# main thread's stack is deep used to abort with "stack pointer ... is not
# within the stack ... that is being switched away from". The collector
# recorded a stale bound for the main thread's stack: a snapshot of the
# stack size soft limit (glibc) or of the currently mapped stack (musl,
# just 132 KiB) taken when the collector was initialised, before Nix
# raises the soft limit. Lowering the soft limit to 1 MiB makes the bug
# reproducible on glibc with a moderate recursion depth.

file="$TEST_ROOT/deep-store-copy"
# Make sure the file isn't in the store yet, so that it has to be copied.
echo "$RANDOM $(date +%s%N)" > "$file"

storePath=$(
    ulimit -S -s 1024
    nix eval --impure --raw --expr "
      let f = n: if n == 0 then \"\${$file}\" else f (n - 1) + \"\"; in f 3000
    "
)

[[ "$storePath" == "${NIX_STORE_DIR:-/nix/store}"/*-deep-store-copy ]]
diff "$file" "$storePath"

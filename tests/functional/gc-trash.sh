#!/usr/bin/env bash

# Test deleting garbage in background threads via a trash directory.

source common.sh

needLocalStore "'gc-delete-threads' is a daemon option"

TODO_NixOS

clearStore

noTrash() {
    [[ -z $(find "$NIX_STORE_DIR" -maxdepth 1 -name '.gc-trash*') ]]
}

trashEntries() {
    find "$NIX_STORE_DIR" -mindepth 2 -maxdepth 2 -path '*/.gc-trash*/*' | wc -l
}

garbageDir=$TEST_ROOT/garbage
rm -rf "$garbageDir"
mkdir -p "$garbageDir"
for i in $(seq 1 10); do
    mkdir -p "$garbageDir/$i/a/b"
    head -c 1048576 /dev/urandom > "$garbageDir/$i/a/b/data"
    ln -s data "$garbageDir/$i/a/b/link"
done

addGarbage() {
    garbage=()
    for i in $(seq 1 10); do
        garbage+=("$(nix store add-path --name "garbage-$i" "$garbageDir/$i")")
    done
}

# The default suits shared or IOPS-capped volumes.
[[ $(nix config show gc-delete-threads) == 4 ]]

# Deleting all garbage concurrently leaves nothing behind.
addGarbage
nix-store --gc --option gc-delete-threads 4
for p in "${garbage[@]}"; do
    [[ ! -e $p ]]
done
noTrash

# `max-freed` is respected exactly, even though paths are deleted in
# the background: each garbage path is a bit bigger than 1 MiB, so
# deleting one is enough.
addGarbage
nix-store --gc --max-freed 1M --option gc-delete-threads 4
remaining=0
for p in "${garbage[@]}"; do
    if [[ -e $p ]]; then remaining=$((remaining + 1)); fi
done
(( remaining == 9 ))
noTrash
nix-store --gc

# `max-freed` is also respected for garbage whose size isn't known in
# advance, such as invalid paths: each one frees 400 bytes, so three of
# them are needed.
for i in $(seq 1 20); do
    d=$NIX_STORE_DIR/$(printf '%032d' "$i")-invalid-$i
    mkdir "$d"
    head -c 400 /dev/zero > "$d/data"
done
nix-store --gc --max-freed 1000 --option gc-delete-threads 8
(( $(find "$NIX_STORE_DIR" -maxdepth 1 -name '*-invalid-*' | wc -l) == 17 ))
noTrash
nix-store --gc

# Trash left behind by an interrupted garbage collection is deleted by
# the next one, also if it's not writable.
staleTrash=$NIX_STORE_DIR/.gc-trash-1234-0
mkdir -p "$staleTrash"
cp -r "$garbageDir/1" "$staleTrash/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-stale-1"
cp -r "$garbageDir/2" "$staleTrash/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-stale-2"
echo foo > "$staleTrash/stale-file"
chmod -R a-w "$staleTrash"
nix-store --gc --option gc-delete-threads 4 2>&1 | tee "$TEST_ROOT/log"
grepQuiet "deleting '$NIX_STORE_DIR/.gc-trash-1234-0' left over by an interrupted garbage collection" "$TEST_ROOT/log"
noTrash

# Without background deletion, stale trash is deleted like other garbage.
mkdir -p "$staleTrash"
cp -r "$garbageDir/1" "$staleTrash/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-stale-1"
chmod -R a-w "$staleTrash"
nix-store --gc --option gc-delete-threads 1
noTrash

# The number of paths in the trash directory is bounded by twice the
# number of threads. Block the background deletions to check this.
addGarbage
fifo=$TEST_ROOT/gc-sync-delete
rm -f "$fifo"
mkfifo "$fifo"
_NIX_TEST_GC_SYNC_DELETE=$fifo nix-store --gc --option gc-delete-threads 2 &
pid=$!
for _ in $(seq 1 100); do
    if (( $(trashEntries) >= 4 )); then break; fi
    sleep 0.1
done
sleep 1
(( $(trashEntries) == 4 ))

# An interrupted garbage collection leaves its trash behind, which the
# next one deletes.
kill -INT "$pid"
while kill -0 "$pid" 2> /dev/null; do
    timeout 1 bash -c "echo > '$fifo'" || true
done
if wait "$pid"; then
    fail "garbage collection should have been interrupted"
fi
(( $(trashEntries) == 4 ))
nix-store --gc --option gc-delete-threads 2 2>&1 | tee "$TEST_ROOT/log"
grepQuiet "left over by an interrupted garbage collection" "$TEST_ROOT/log"
for p in "${garbage[@]}"; do
    [[ ! -e $p ]]
done
noTrash

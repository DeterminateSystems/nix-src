#!/usr/bin/env bash

# Tests for speculative evaluation in the multi-threaded evaluator
# (`eval-speculation-threshold`).

source common.sh

# A never-demanded attribute with a substantial body that throws must
# not affect the result, even though it is evaluated speculatively
# (its error is stored in the thunk and only reported on demand).
expr='
let
  big = n: builtins.foldl'"'"' (a: b: a + b) 0 (builtins.genList (i: i * n) 100);
  xs = builtins.genList (i: {
    a = big i + big (i + 1) + big (i + 2) + big (i + 3);
    b = if i >= 0 then throw ("never demanded " + toString i) else big i;
  }) 50;
in
  builtins.foldl'"'"' (a: x: a + x.a) 0 xs
'

expected=$(nix eval --eval-cores 1 --expr "$expr")

# Maximal speculation: every thunk with a body of at least one node.
actual=$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/stats.json" nix eval --eval-cores 4 --eval-speculation-threshold 1 --expr "$expr")
[[ "$actual" = "$expected" ]]

# Something was actually speculated.
speculated=$(jq .nrThunksSpeculated "$TEST_ROOT/stats.json")
[[ "$speculated" -gt 0 ]]

# Speculating a throwing thunk doesn't hide the error when it *is*
# demanded.
expectStderr 1 nix eval --eval-cores 4 --eval-speculation-threshold 1 --expr '
let
  xs = builtins.genList (i: { a = if i > 100 then i else throw ("demanded " + toString i); }) 5;
in
  builtins.foldl'"'"' (a: x: a + x.a) 0 xs
' | grepQuiet "demanded 0"

# The setting can be disabled.
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/stats0.json" nix eval --eval-cores 4 --eval-speculation-threshold 0 --expr "$expr" > /dev/null
[[ "$(jq .nrThunksSpeculated "$TEST_ROOT/stats0.json")" = 0 ]]

# Import prefetching: a list of path literals (like a NixOS module
# list) gets its files parsed and evaluated ahead of time. Non-existent
# and non-Nix files in the list must be harmless.
mkdir -p "$TEST_ROOT/mods"
for i in $(seq 0 9); do
    echo "{ x }: x + $i" > "$TEST_ROOT/mods/m$i.nix"
done
echo "not nix" > "$TEST_ROOT/mods/data.txt"
cat > "$TEST_ROOT/mods/default.nix" <<EOF
let
  mods = [ ./m0.nix ./m1.nix ./m2.nix ./m3.nix ./m4.nix ./m5.nix ./m6.nix ./m7.nix ./m8.nix ./m9.nix ./data.txt ./does-not-exist.nix ];
  imported = builtins.filter builtins.isFunction (map (m: if builtins.pathExists m && builtins.match ".*[.]nix" (toString m) != null then import m else null) mods);
in
  builtins.foldl' (a: f: f { x = a; }) 0 imported
EOF
expected=$(nix eval --eval-cores 1 --file "$TEST_ROOT/mods")
actual=$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/stats2.json" nix eval --eval-cores 4 --eval-speculation-threshold 1 --file "$TEST_ROOT/mods")
[[ "$actual" = "$expected" ]]
[[ "$(jq .nrImportsPrefetched "$TEST_ROOT/stats2.json")" -ge 10 ]]

# It has no effect in single-threaded mode.
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/stats1.json" nix eval --eval-cores 1 --eval-speculation-threshold 1 --expr "$expr" > /dev/null
[[ "$(jq .nrThunksSpeculated "$TEST_ROOT/stats1.json")" = 0 ]]

#!/usr/bin/env bash

source common.sh

if [[ $(nix eval --extra-experimental-features wasm-builtin --expr 'builtins ? wasm') = false ]]; then
    skipTest "builtins.wasm not available"
fi

# Test running a WebAssembly module in text format (WAT).
[[ $(nix eval --json --impure \
    --extra-experimental-features wasm-builtin \
    --expr "builtins.wasm { wat = builtins.readFile ./fib.wat; function = \"fib\"; } 40") = 165580141 ]]

# Test running a WebAssembly module in binary format (.wasm).
[[ $(nix eval --json --impure \
    --extra-experimental-features wasm-builtin \
    --expr "builtins.wasm { path = ./fib.wasm; function = \"fib\"; } 40") = 165580141 ]]

# A host function called with an out-of-range pointer must fail with an
# error rather than access memory outside the Wasm memory.
expectStderr 1 nix eval --impure \
    --extra-experimental-features wasm-builtin \
    --expr "builtins.wasm { wat = builtins.readFile ./oob.wat; function = \"oob\"; } 0" \
    | grepQuiet "Wasm memory access out of bounds"

# Test copying an attrset into Wasm memory with `get_attrset`. The
# attributes must be returned in lexicographically sorted order, both when
# the guest-supplied buffer is used and when the host has to allocate one
# via `nix_wasm_alloc`. The right-hand side of `//` is a layered attrset.
for function in attrs_inline attrs_alloc; do
    [[ $(nix eval --json --impure \
        --extra-experimental-features wasm-builtin \
        --expr "builtins.wasm { wat = builtins.readFile ./attrset.wat; function = \"$function\"; } ({ b = 1; a = 2; } // { \"a b\" = 3; })") = '["a",2,"a b",3,"b",1]' ]]

    [[ $(nix eval --json --impure \
        --extra-experimental-features wasm-builtin \
        --expr "builtins.wasm { wat = builtins.readFile ./attrset.wat; function = \"$function\"; } { }") = '[]' ]]
done

# A `nix_wasm_alloc` export that doesn't have type `(i32, i32) -> i32` must
# be rejected.
expectStderr 1 nix eval --impure \
    --extra-experimental-features wasm-builtin \
    --expr "builtins.wasm { wat = builtins.readFile ./bad-alloc.wat; function = \"id\"; } 0" \
    | grepQuiet "does not have type '(size: i32, align: i32) -> i32'"

# Evaluate a file of parallel Wasm calls, check its result, and check
# that some fibers were actually suspended while waiting (otherwise the
# test would pass trivially).
checkParallelWasm() {
    local file="$1" expected="$2" out
    out=$(NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/wasm-stats.json" \
        timeout 120 nix eval --json --eval-cores 4 \
        --extra-experimental-features 'wasm-builtin parallel-eval' \
        --file "$file") || fail "evaluating $file failed or timed out (exit code $?)"
    [[ "$out" = "$expected" ]] || fail "evaluating $file returned '$out' instead of '$expected'"
    (( $(jq .maxSuspendedFibers "$TEST_ROOT/wasm-stats.json") > 0 )) \
        || fail "evaluating $file did not suspend any fibers"
}

# Many parallel Wasm calls that all need a value that takes a while to
# evaluate. The fibers making these calls are suspended in the middle of
# their Wasm calls, and other fibers start or resume Wasm calls on the
# same thread in the meantime.
cat > "$TEST_ROOT/wasm-parallel.nix" <<NIX
let
  shared = builtins.foldl' builtins.add 10 (builtins.genList (x: 0) 3000000);
  xs = builtins.genList (i: builtins.wasm { path = $PWD/fib.wasm; function = "fib"; } shared) 64;
in
builtins.parallel xs (builtins.foldl' builtins.add 0 xs)
NIX

checkParallelWasm "$TEST_ROOT/wasm-parallel.nix" $((64 * 89))

# Like the above, but with the Wasm calls nested: `apply` calls back into
# Nix, which calls `apply` again, which calls `fib` on the shared value.
# So the fibers are suspended with three Wasm calls active.
cat > "$TEST_ROOT/wasm-parallel-nested.nix" <<NIX
let
  shared = builtins.foldl' builtins.add 10 (builtins.genList (x: 0) 3000000);
  apply = f: x: builtins.wasm { wat = builtins.readFile $PWD/apply.wat; function = "apply"; } { inherit f x; };
  fib = builtins.wasm { path = $PWD/fib.wasm; function = "fib"; };
  xs = builtins.genList (i: apply (apply fib) shared) 64;
in
builtins.parallel xs (builtins.foldl' builtins.add 0 xs)
NIX

checkParallelWasm "$TEST_ROOT/wasm-parallel-nested.nix" $((64 * 89))

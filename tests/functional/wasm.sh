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

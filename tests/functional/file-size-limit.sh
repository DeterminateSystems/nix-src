#!/usr/bin/env bash

source common.sh

# Nix ignores SIGXFSZ, so a write that exceeds the file size limit
# (`ulimit -f`) is an error rather than killing Nix.
head -c 1000000 /dev/zero > "$TEST_ROOT/big"
# shellcheck disable=SC2016
expectStderr 1 bash -c 'ulimit -f 100; nix nar pack "$1" > "$2"' _ "$TEST_ROOT/big" "$TEST_ROOT/big.nar" \
    | grepQuiet "File too large"

# Child processes such as builders don't inherit the ignored SIGXFSZ,
# so here it kills the builder's subshell.
# shellcheck disable=SC2016
outPath=$(nix-build --no-out-link -E '
  with import '"${config_nix}"';
  mkDerivation {
    name = "file-size-limit";
    buildCommand = "(ulimit -f 1; head -c 4096 /dev/zero > big) || echo $? > $out";
  }
')
[[ $(< "$outPath") = $((128 + $(kill -l XFSZ))) ]]

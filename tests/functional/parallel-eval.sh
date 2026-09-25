#!/usr/bin/env bash

# Tests for the multi-threaded evaluator (`eval-cores`).

source common.sh

# `builtins.addErrorContext` evaluates its message after the body has
# thrown. If the message depends on a thunk that another fiber is
# evaluating, the current fiber has to wait for it. That must not
# happen inside the exception handler (which the fiber cannot suspend
# in), so the message is evaluated after leaving the handler.
expectStderr 1 nix eval --eval-cores 4 --json --expr '
let
  big = builtins.foldl'"'"' (a: b: a + b) 0 (builtins.genList (x: x) 3000000);
in
{
  a = big;
  b = builtins.addErrorContext "context mentioning ${toString big}" (throw "boom");
}
' | grepQuiet "context mentioning 4499998500000"

#!/usr/bin/env bash

# Tests for the multi-threaded evaluator (`eval-cores`).

source common.sh

# `builtins.addErrorContext` evaluates its message after the body has
# thrown. If the message depends on a thunk that another fiber is
# evaluating, the current fiber has to wait for it. That must not
# happen inside the exception handler (which the fiber cannot suspend
# in), so the message is evaluated after leaving the handler.
cat > "$TEST_ROOT/add-error-context.nix" <<'NIX'
let
  big = builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 3000000);
in
{
  a = big;
  b = builtins.addErrorContext "context mentioning ${toString big}" (throw "boom");
}
NIX

expectStderr 1 nix eval --eval-cores 4 --json --file "$TEST_ROOT/add-error-context.nix" | grepQuiet "context mentioning 4499998500000"

# Likewise, the error trace of an attribute selection shows the
# attribute path, which evaluates dynamic attribute names that weren't
# reached before the error (here `${name}`), so this must also happen
# outside of the exception handler.
cat > "$TEST_ROOT/select-dynamic-attr.nix" <<'NIX'
let
  name = toString (builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 3000000));
  a = { b = throw "boom"; };
in
{
  x = name;
  y = a.b.c.${name};
}
NIX

expectStderr 1 nix eval --eval-cores 4 --json --file "$TEST_ROOT/select-dynamic-attr.nix" | grepQuiet "while evaluating the attribute 'b.c.\"4499998500000\"'"

# A stack overflow on a fiber must be reported as such, rather than
# killing Nix with SIGSEGV. This requires the worker thread running the
# fiber to have an alternate signal stack. (The stack overflow handler
# only recognises overflows on x86_64 Linux.)
if [[ $(uname -ms) = "Linux x86_64" ]]; then
    expectStderr 1 nix eval --eval-cores 4 --max-call-depth 10000000 --json --file ./fiber-stack-overflow.nix | grepQuiet "stack overflow (possible infinite recursion)"
fi

# `builtins.memoise` must call the function only once for each distinct
# argument, even if many fibers call it concurrently. The arguments are
# structurally equal but not identical, so comparing them takes a while,
# making it likely that fibers race to add the same entry.
cat > "$TEST_ROOT/memoise.nix" <<'NIX'
let
  f = args: builtins.trace "computing ${toString args.n}" (args.n * 2);
in
builtins.genList (i: builtins.memoise f {
  n = i - (i / 64) * 64;
  big = builtins.genList (x: { inherit x; }) 1000;
}) 4000
NIX

NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH="$TEST_ROOT/memoise-stats.json" \
    nix eval --extra-experimental-features memoise --eval-cores 16 --json --file "$TEST_ROOT/memoise.nix" \
    > "$TEST_ROOT/memoise.out" 2> "$TEST_ROOT/memoise.err"
[[ $(jq add "$TEST_ROOT/memoise.out") = 250976 ]]
[[ $(grep -c "computing" "$TEST_ROOT/memoise.err") = 64 ]]
[[ $(jq .nrMemoiseMisses "$TEST_ROOT/memoise-stats.json") = 64 ]]
[[ $(jq .nrMemoiseHits "$TEST_ROOT/memoise-stats.json") = 3936 ]]

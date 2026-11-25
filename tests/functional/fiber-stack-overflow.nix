# Overflows the stack of a fiber of an evaluator worker thread when
# evaluated with `nix eval --eval-cores 4 --json`. `b` is a chain of
# thunks that each select an attribute from the previous one. Selections
# count towards `max-call-depth`, so this needs a `max-call-depth` above
# 1,000,000 to overflow the stack. `a` keeps the main thread busy, so
# that `b` overflows on a worker.
{
  a = builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 1000000);
  b = (builtins.foldl' (acc: _: { x = acc.x; }) { x = 1; } (builtins.genList (x: x) 1000000)).x;
}

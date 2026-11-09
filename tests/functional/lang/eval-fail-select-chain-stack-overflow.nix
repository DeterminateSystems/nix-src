# Test that a long chain of attribute selections through thunks (as in
# overlays doing `x = prev.x`) produces a controlled stack overflow error
# rather than a segfault.

let
  chain = builtins.foldl' (acc: _: { x = acc.x; }) { x = 1; } (builtins.genList (x: x) 1000000);
in
chain.x

let
  r = rec {
    __overrides = {
      a = 2;
      c = 3;
    };
    a = 1;
    b = a;
    ${"d" + toString a} = b;
    ${null} = throw "not evaluated";
  };
in
{
  names = builtins.attrNames r;
  inherit (r)
    a
    b
    c
    d2
    ;
}

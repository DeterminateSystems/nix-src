# Tests for the `//` operator in the cases where the operands are
# merged into a new attribute set rather than "layered", i.e. when the
# right operand has more than 16 attributes or the left operand
# already consists of the maximum number of layers.
let
  # An attribute set with attributes `a<from>` ... `a<to - 1>`.
  mk =
    tag: from: to:
    builtins.listToAttrs (
      builtins.genList (i: {
        name = "a${toString (from + i)}";
        value = "${tag}${toString (from + i)}";
      }) (to - from)
    );

  show = s: builtins.concatStringsSep " " (map (n: "${n}=${toString s.${n}}") (builtins.attrNames s));

  line = n: s: (builtins.unsafeGetAttrPos n s).line;

  # Literal sets, so that the attributes have distinct positions.
  left = {
    a = "l";
    b = "l";
    c = "l";
  };

  subset = {
    b = "l";
    s = "l";
  };

  # This has more than 16 attributes.
  right = {
    b = "r";
    c = "r";
    d = "r";
    e = "r";
    f = "r";
    g = "r";
    h = "r";
    i = "r";
    j = "r";
    k = "r";
    l = "r";
    m = "r";
    n = "r";
    o = "r";
    p = "r";
    q = "r";
    r = "r";
    s = "r";
  };

  sameNames = builtins.mapAttrs (n: v: "m") right;

  # Sets consisting of the maximum number of layers at some point.
  chain = builtins.foldl' (acc: i: acc // { "c${toString i}" = i; }) { base = 0; } (
    builtins.genList (i: i) 20
  );

  chainOverlap = builtins.foldl' (
    acc: i:
    acc
    // {
      x = i;
      "c${toString i}" = i;
    }
  ) { x = "base"; } (builtins.genList (i: i) 20);

  chainSame = builtins.foldl' (acc: i: acc // { x = i; }) { x = "base"; } (
    builtins.genList (i: i) 20
  );

  # A layered set with more than 16 attributes.
  layered = mk "b" 0 20 // { y1 = 1; } // { y2 = 2; } // { a5 = "y"; };
in
{
  partial = show (left // right);
  partialLines = map (n: line n (left // right)) [
    "a"
    "b"
    "c"
    "s"
  ];

  subset = show (subset // right);
  subsetIsRight = (subset // right) == right;
  subsetLines = map (n: line n (subset // right)) [
    "b"
    "s"
  ];

  sameNames = show (right // sameNames);
  sameNamesReverse = show (sameNames // right);
  sameNamesLines = map (n: line n (sameNames // right)) [
    "b"
    "s"
  ];

  rightIntoLeft = show (right // left);

  disjoint = show (mk "l" 0 20 // mk "r" 100 120);
  similarSize = show (mk "l" 0 40 // mk "r" 20 60);
  bigLeft = show (mk "l" 0 300 // mk "r" 290 307);
  bigLeftDisjoint = builtins.length (builtins.attrNames (mk "l" 0 300 // mk "r" 400 417));
  bigRight = builtins.length (builtins.attrNames (mk "l" 290 307 // mk "r" 0 300));
  bigRightValue = (mk "l" 290 307 // mk "r" 0 300).a295;
  bigRightKept = (mk "l" 290 307 // mk "r" 0 300).a305;

  chain = show chain;
  chainOverlap = show chainOverlap;
  chainSame = show chainSame;
  chainBigRight = show (chainOverlap // right);

  layeredRight = show (mk "l" 10 40 // layered);
  layeredLeft = show (layered // mk "r" 10 30);
  layeredBoth = show (layered // (mk "r" 15 35 // { y2 = "z"; }));
}

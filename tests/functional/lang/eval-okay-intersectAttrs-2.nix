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
    d = "l";
  };

  right = {
    b = "r";
    d = "r";
    e = "r";
  };

  sameNames = {
    a = "s";
    b = "s";
    c = "s";
    d = "s";
  };

  subset = {
    c = "t";
    a = "t";
  };

  # Layered sets.
  layeredLeft = left // { e = "ll"; } // { b = "ll"; };
  layeredRight = right // { a = "lr"; } // { d = "lr"; };
in
{
  # The result is empty.
  disjoint = builtins.intersectAttrs left { x = 1; };
  disjointBig = builtins.intersectAttrs (mk "l" 0 50) (mk "r" 50 100);
  emptyLeft = builtins.intersectAttrs { } right;
  emptyRight = builtins.intersectAttrs left { };
  emptyBoth = builtins.intersectAttrs { } { };

  # The result is the second argument.
  sameNames = show (builtins.intersectAttrs left sameNames);
  sameNamesIsRight = builtins.intersectAttrs left sameNames == sameNames;
  sameNamesLines = map (n: line n (builtins.intersectAttrs left sameNames)) [
    "a"
    "d"
  ];
  subset = show (builtins.intersectAttrs left subset);
  subsetLines = map (n: line n (builtins.intersectAttrs left subset)) [
    "a"
    "c"
  ];
  self = show (builtins.intersectAttrs left left);
  bigSubset = show (builtins.intersectAttrs (mk "l" 0 100) (mk "r" 40 45));

  # The result is part of the second argument.
  partial = show (builtins.intersectAttrs left right);
  partialLines = map (n: line n (builtins.intersectAttrs left right)) [
    "b"
    "d"
  ];
  partialReverse = show (builtins.intersectAttrs right left);
  partialReverseLines = map (n: line n (builtins.intersectAttrs right left)) [
    "b"
    "d"
  ];
  smallLeft = show (builtins.intersectAttrs (mk "l" 45 55) (mk "r" 0 50));
  smallRight = show (builtins.intersectAttrs (mk "l" 0 50) (mk "r" 45 55));

  # More results than fit in a small buffer.
  big = show (builtins.intersectAttrs (mk "l" 0 150) (mk "r" 60 200));
  bigReverse = builtins.length (
    builtins.attrNames (builtins.intersectAttrs (mk "l" 60 200) (mk "r" 0 150))
  );

  layeredLeft = show (builtins.intersectAttrs layeredLeft right);
  layeredRight = show (builtins.intersectAttrs left layeredRight);
  layeredBoth = show (builtins.intersectAttrs layeredLeft layeredRight);
  layeredRightAll = show (builtins.intersectAttrs (left // { e = 1; }) layeredRight);
  layeredBig = show (
    builtins.intersectAttrs (mk "l" 0 30 // { a5 = "x"; } // { z = "x"; }) (
      mk "r" 20 40 // { a25 = "y"; } // { z = "y"; }
    )
  );
}

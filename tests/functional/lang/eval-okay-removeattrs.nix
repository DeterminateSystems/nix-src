let
  # An attribute set with attributes `a<from>` ... `a<to - 1>`.
  mk =
    from: to:
    builtins.listToAttrs (
      builtins.genList (i: {
        name = "a${toString (from + i)}";
        value = from + i;
      }) (to - from)
    );

  names = from: to: builtins.genList (i: "a${toString (from + i)}") (to - from);

  show = s: builtins.concatStringsSep " " (map (n: "${n}=${toString s.${n}}") (builtins.attrNames s));

  line = n: s: (builtins.unsafeGetAttrPos n s).line;

  # A literal set, so that the attributes have distinct positions.
  set = {
    a = 1;
    b = 2;
    c = 3;
    d = 4;
  };

  # A layered set.
  layered = set // { e = 5; } // { b = 20; } // { f = 6; };
in
{
  some = show (
    removeAttrs set [
      "b"
      "d"
    ]
  );
  someLines = map (n: line n (removeAttrs set [ "b" ])) [
    "a"
    "c"
    "d"
  ];

  noneAbsent = show (
    removeAttrs set [
      "x"
      "y"
    ]
  );
  noneAbsentIsSet = removeAttrs set [ "x" ] == set;
  noneAbsentLines = map (n: line n (removeAttrs set [ "x" ])) [
    "a"
    "d"
  ];
  noneEmptyList = show (removeAttrs set [ ]);

  all = removeAttrs set [
    "a"
    "b"
    "c"
    "d"
  ];
  allAndMore = removeAttrs set [
    "d"
    "x"
    "c"
    "b"
    "a"
    "a"
  ];

  duplicates = show (
    removeAttrs set [
      "c"
      "a"
      "c"
      "c"
      "a"
    ]
  );
  mixed = show (
    removeAttrs set [
      "z"
      "c"
      "0"
      "aa"
    ]
  );

  # A short list and a big set.
  bigSetSome = show (
    removeAttrs (mk 0 60) [
      "a7"
      "a59"
      "a7"
      "a60"
    ]
  );
  bigSetNone = builtins.length (builtins.attrNames (removeAttrs (mk 0 60) [ "b" ]));
  bigLayeredSome = show (
    removeAttrs (mk 0 60 // { e = 5; } // { a7 = "x"; }) [
      "a7"
      "e"
      "x"
    ]
  );

  # A long list.
  longListSome = show (removeAttrs (mk 0 40) (names 10 35));
  longListNone = builtins.length (builtins.attrNames (removeAttrs (mk 0 40) (names 100 130)));
  longListAll = removeAttrs (mk 0 40) (names 0 50);
  longListDuplicates = show (removeAttrs (mk 0 40) (names 5 40 ++ names 3 30));
  longListSmallSet = show (removeAttrs set (names 0 30 ++ [ "c" ]));

  layeredSome = show (
    removeAttrs layered [
      "b"
      "f"
      "x"
    ]
  );
  layeredNone = show (removeAttrs layered [ "x" ]);
  layeredAll = removeAttrs layered (builtins.attrNames layered);

  empty = removeAttrs { } [ "a" ];
  emptyBoth = removeAttrs { } [ ];
}

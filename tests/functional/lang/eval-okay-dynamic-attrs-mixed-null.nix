let
  yes = "c";
  no = null;

  plain = {
    a = 1;
    ${no} = throw "not evaluated";
    ${yes} = 3;
    ${if true then null else "x"} = 4;
    "${"b" + ""}" = 2;
    ${null} = 5;
    z = 26;
  };

  recursive = rec {
    a = "d";
    ${a} = b;
    ${no} = throw "not evaluated";
    b = 2;
    ${a + "e"} = b + 1;
  };

  allNull = {
    ${no} = 1;
    ${null} = 2;
  };
in
{
  inherit plain recursive;
  plainNames = builtins.attrNames plain;
  recursiveNames = builtins.attrNames recursive;
  allNullIsEmpty = allNull == { };
  allNullNames = builtins.attrNames allNull;
}

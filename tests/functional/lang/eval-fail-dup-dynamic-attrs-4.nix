# A dynamic attribute that duplicates an attribute added by `__overrides`.
rec {
  __overrides = {
    c = 3;
  };
  a = 1;
  ${"c" + ""} = 2;
}

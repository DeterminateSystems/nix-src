# A dynamic attribute that duplicates a static one. The error must be
# raised before the names of later dynamic attributes are evaluated.
{
  a = 1;
  ${"a" + ""} = 2;
  ${throw "later name must not be evaluated"} = 3;
}

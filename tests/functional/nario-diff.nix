{
  version,
}:
with import ./config.nix;

let

  # A dependency that's identical in both versions.
  shared = mkDerivation {
    name = "nario-shared-1.0";
    buildCommand = "mkdir $out; echo shared > $out/shared";
  };

  # A dependency that only exists in version 2, so it has no diff base.
  extra = mkDerivation {
    name = "nario-extra";
    buildCommand = "mkdir $out; seq 1 10000 > $out/data";
  };

in

mkDerivation {
  name = "nario-pkg-${version}";
  buildCommand = ''
    mkdir $out
    seq 1 100000 > $out/data
    echo ${version} >> $out/data
    echo $out >> $out/data
    ln -s ${shared} $out/shared
    ${if version == "2" then "ln -s ${extra} $out/extra" else ""}
  '';
}

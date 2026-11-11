{
  pkgs,
  nix,
}:
let
  # Exercise a client that cannot negotiate named-activities.
  client = pkgs.rustPlatform.buildRustPackage {
    pname = "harmonia-named-activities";
    version = "0.1.0";
    src = ./.;
    cargoHash = "sha256-c5+oZDKz9fYzexEdWxnM/ttEgjjmkbTPmfoG/3AdocU=";
    doCheck = false;
  };
in
pkgs.runCommand "harmonia-named-activities" { } ''
  ${client}/bin/harmonia-named-activities ${nix}/bin/nix
  touch "$out"
''

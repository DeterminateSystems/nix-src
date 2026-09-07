{
  inputs.nix.url = "../..";
  inputs.nix.inputs.nixpkgs.url = "https://flakehub.com/f/DeterminateSystems/secure-packages-26.05/0";

  outputs = { self, nix }: nix.outputs;
}

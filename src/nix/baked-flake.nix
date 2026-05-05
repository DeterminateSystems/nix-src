{
  outputs =
    { self }:
    let
      data = builtins.fromJSON (builtins.readFile ./outputs.json);

      convert =
        output:
        if output ? children then
          builtins.mapAttrs (childName: child: convert child) output.children
        else if output ? "derivation" then
          {
            type = "derivation";
            name = output.derivation.name;
            system = builtins.head output.forSystems; # FIXME
            meta.description = output.shortDescription;
            drvPath = builtins.fakeDerivation {
              name = output.derivation.name;
              outputs = builtins.mapAttrs (outputName: output: { path = output; }) output.derivation.outputs;
            };
            outPath = output.derivation.outputs.out; # FIXME
            outputName = "out"; # FIXME
          }
        else
          throw "Output not supported in a baked flake.";
    in
    builtins.mapAttrs (outputName: output: convert output.output) data;
}

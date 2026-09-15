{
  outputs =
    { self }:
    let
      data = builtins.fromJSON (builtins.readFile ./outputs.json);

      cleanup = builtins.filterAttrs (name: value: value != { });

      convert =
        output:
        if output ? children then
          cleanup (builtins.mapAttrs (childName: child: convert child) output.children)
        else if output ? "derivation" then
          let
            baked = builtins.bakedDerivation {
              name = output.derivation.name;
              outputs = builtins.mapAttrs (outputName: path: { inherit path; }) output.derivation.outputs;
            };
          in
          {
            type = "derivation";
            name = output.derivation.name;
            system = builtins.head output.forSystems; # FIXME
            meta.description = output.shortDescription;
            drvPath = baked.drvPath;
            outPath = baked.out; # FIXME
            outputName = "out"; # FIXME
          }
        else
          {
          };
    in
    cleanup (builtins.mapAttrs (outputName: output: convert (output.output or { })) data);
}

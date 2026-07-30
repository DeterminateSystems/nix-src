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
          {
          };
    in
    cleanup (builtins.mapAttrs (outputName: output: convert output.output) data);
}

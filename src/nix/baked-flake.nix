{
  outputs =
    { self }:
    let
      data = builtins.fromJSON (builtins.readFile ./outputs.json);

      cleanup = builtins.filterAttrs (name: value: value != { });

      # `setAttrByPath [ "a" "b" ] x` returns `{ a.b = x; }`.
      setAttrByPath =
        path: value:
        if path == [ ] then
          value
        else
          { ${builtins.head path} = setAttrByPath (builtins.tail path) value; };

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
            outputNames = builtins.attrNames output.derivation.outputs;

            # The output that this attribute refers to.
            outputName = output.derivation.outputName or (builtins.head outputNames);

            # Mirror the attribute set produced by `derivation`: the common
            # attributes, plus one attribute per output.
            commonAttrs = {
              type = "derivation";
              name = output.derivation.name;
              system = output.derivation.system or (builtins.head output.forSystems);
              meta =
                (if output ? shortDescription then { description = output.shortDescription; } else { })
                // (
                  if output.derivation ? mainProgram then { mainProgram = output.derivation.mainProgram; } else { }
                );
              drvPath = baked.drvPath;
              outputs = outputNames;
              all = map (outputName: outputAttrs.${outputName}) outputNames;
            }
            // outputAttrs;

            outputAttrs = builtins.listToAttrs (
              map (outputName: {
                name = outputName;
                value = commonAttrs // {
                  inherit outputName;
                  outPath = baked.${outputName};
                };
              }) outputNames
            );

            drv = outputAttrs.${outputName};
          in
          # The derivation may live at a sub-path of the output attribute
          # (e.g. `config.system.build.toplevel` for `nixosConfigurations`).
          setAttrByPath (output.derivationAttrPath or [ ]) drv
        else
          { };
    in
    cleanup (builtins.mapAttrs (outputName: output: convert (output.output or { })) data);
}

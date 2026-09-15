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
            drv = {
              type = "derivation";
              name = output.derivation.name;
              system = builtins.head output.forSystems; # FIXME
              meta = if output ? shortDescription then { description = output.shortDescription; } else { };
              drvPath = baked.drvPath;
              outPath = baked.out; # FIXME
              outputName = "out"; # FIXME
            };
          in
          # The derivation may live at a sub-path of the output attribute
          # (e.g. `config.system.build.toplevel` for `nixosConfigurations`).
          setAttrByPath (output.derivationAttrPath or [ ]) drv
        else
          { };
    in
    cleanup (builtins.mapAttrs (outputName: output: convert (output.output or { })) data);
}

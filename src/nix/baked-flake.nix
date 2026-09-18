{
  outputs =
    { self }:
    let
      data = builtins.fromJSON (builtins.readFile ./outputs.json);

      optionalAttrs = cond: attrs: if cond then attrs else { };

      # `setAttrByPath [ "a" "b" ] x` returns `{ a.b = x; }`.
      setAttrByPath =
        path: value:
        if path == [ ] then
          value
        else
          { ${builtins.head path} = setAttrByPath (builtins.tail path) value; };

      # Prune the inventory to the nodes that can be baked: derivation
      # leaves, and non-leaf nodes that contain at least one of them.
      prune =
        node:
        if node ? children then
          let
            children = builtins.filterAttrs (name: child: child != null) (
              builtins.mapAttrs (name: child: prune child) node.children
            );
          in
          if children == { } then null else node // { inherit children; }
        else if node ? derivation then
          node
        else
          null;

      inventories = builtins.filterAttrs (name: node: node != null) (
        builtins.mapAttrs (name: output: prune (output.output or null)) data
      );

      # Convert an inventory node into the corresponding flake output.
      convert =
        node:
        if node ? children then
          builtins.mapAttrs (name: child: convert child) node.children
        else
          let
            baked = builtins.bakedDerivation {
              name = node.derivation.name;
              outputs = builtins.mapAttrs (outputName: path: { inherit path; }) node.derivation.outputs;
            };

            outputNames = builtins.attrNames node.derivation.outputs;

            # The output that this attribute refers to.
            outputName = node.derivation.outputName or (builtins.head outputNames);

            # Mirror the attribute set produced by `derivation`: the common
            # attributes, plus one attribute per output.
            commonAttrs = {
              type = "derivation";
              name = node.derivation.name;
              system = node.derivation.system or (builtins.head node.forSystems);
              meta =
                optionalAttrs (node ? shortDescription) { description = node.shortDescription; }
                // optionalAttrs (node.derivation ? mainProgram) { mainProgram = node.derivation.mainProgram; };
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
          in
          # The derivation may live at a sub-path of the output attribute
          # (e.g. `config.system.build.toplevel` for `nixosConfigurations`).
          setAttrByPath (node.derivationAttrPath or [ ]) outputAttrs.${outputName};

      # Convert an inventory node into a flake schema inventory node. The
      # original flake's schemas may need attributes that aren't present
      # in the baked flake (e.g. `pkgs.stdenv.system` for NixOS
      # configurations), so the baked flake provides its own schemas that
      # just reproduce the recorded inventory.
      mkInventory =
        node:
        optionalAttrs (node ? forSystems) { inherit (node) forSystems; }
        // optionalAttrs (node ? isLegacy) { inherit (node) isLegacy; }
        // (
          if node ? children then
            { children = builtins.mapAttrs (name: child: mkInventory child) node.children; }
          else
            {
              derivationAttrPath = node.derivationAttrPath or [ ];
            }
            // optionalAttrs (node ? what) { inherit (node) what; }
            // optionalAttrs (node ? shortDescription) { inherit (node) shortDescription; }
            // optionalAttrs (node ? isFlakeCheck) { inherit (node) isFlakeCheck; }
        );

      schemas = builtins.mapAttrs (
        name: node:
        let
          schema = data.${name};
        in
        {
          version = 1;
          inherit (schema) doc;
          inventory = output: mkInventory node;
        }
        // optionalAttrs (schema ? roles) {
          roles = builtins.listToAttrs (
            map (role: {
              name = role;
              value = { };
            }) schema.roles
          );
        }
        // optionalAttrs (schema ? appendSystem) { inherit (schema) appendSystem; }
        // optionalAttrs (schema ? defaultAttrPath) { inherit (schema) defaultAttrPath; }
      ) inventories;
    in
    builtins.mapAttrs (name: node: convert node) inventories
    // {
      schemas = schemas // {
        schemas = {
          version = 1;
          doc = "The `schemas` flake output defines the flake schemas of this flake.";
          inventory = output: {
            children = builtins.mapAttrs (name: schema: { what = "flake schema"; }) output;
          };
        };
      };
    };
}

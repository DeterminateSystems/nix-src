# The flake providing default schemas.
defaultSchemasFlake:

# The flake whose contents we want to extract.
flake:

# Options to be passed to flake outputs.
options:

let

  # Helper functions.

  mapAttrsToList = f: attrs: map (name: f name attrs.${name}) (builtins.attrNames attrs);

  decorateOutput =
    node: output:
    node
    // (
      if node ? children then
        {
          children = builtins.mapAttrs (name: value: decorateOutput value output.${name}) node.children;
        }
      else
        { }
    )
    // {
      raw = if node ? applyOptions then builtins.applyOptions node options else output;
    }
    // (
      if node ? derivation && node ? applyOptions then
        { derivation = builtins.applyOptions node options; }
      else
        { }
    );

in

rec {
  outputNames = builtins.attrNames flake.outputs;

  allSchemas = (flake.outputs.schemas or defaultSchemasFlake.schemas) // schemaOverrides;

  schemaOverrides = { }; # FIXME

  schemas = builtins.listToAttrs (
    builtins.concatLists (
      mapAttrsToList (
        outputName: output:
        if allSchemas ? ${outputName} then
          [
            {
              name = outputName;
              value = allSchemas.${outputName};
            }
          ]
        else
          [ ]
      ) flake.outputs
    )
  );

  outputs = flake.outputs;

  inventory = builtins.mapAttrs (
    outputName: output:
    if schemas ? ${outputName} && schemas.${outputName}.version == 1 then
      let
        schema = schemas.${outputName};
      in
      schema
      // {
        node = decorateOutput (schemas.${outputName}.inventory output) output;
      }
    else
      {
        unknown = true;
        node = {
          raw = output;
        };
      }
  ) outputs;
}

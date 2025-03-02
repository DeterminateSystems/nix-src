# The flake providing default schemas.
defaultSchemasFlake:

# The flake whose contents we want to extract.
flake:

# Options to be passed to flake outputs.
options:

let

  # Helper functions.

  mapAttrsToList = f: attrs: map (name: f name attrs.${name}) (builtins.attrNames attrs);

  checkOptions =
    declarations: definitions:
    assert builtins.all (
      name: if declarations ? ${name} then true else throw "Flake option `${name}` is not declared."
    ) (builtins.attrNames definitions);
    builtins.mapAttrs (
      name:
      {
        type,
        default,
        description,
      }:
      let
        value = definitions.${name} or default;
      in
      if type.check value then
        value
      else
        throw "Flake option `${name}` does not have expected type `${type.description or "unknown"}`."
    ) declarations;

  decorateOutput =
    inventory: output:
    inventory
    // (
      if inventory ? children then
        {
          children = builtins.mapAttrs (name: value: decorateOutput value output.${name}) inventory.children;
        }
      else
        { }
    )
    // {
      raw = output;
    }
    // (
      if inventory ? derivation && inventory ? applyOptions then
        { derivation = inventory.applyOptions (checkOptions (inventory.options or { }) options); }
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

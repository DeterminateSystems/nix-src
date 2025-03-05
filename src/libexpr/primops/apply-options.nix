node: definitions:
assert builtins.all (
  name: if node.options ? ${name} then true else throw "Flake option `${name}` is not declared."
) (builtins.attrNames definitions);
node.applyOptions (
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
  ) node.options
)

# This is a helper to callFlake() to lazily fetch flake inputs.

# An external value wrapping the C++ `LockedFlake` object.
lockedFlake:

# A primop that, given the locked flake and the input attribute path
# of an input, returns an attribute set mapping the names of its
# inputs to either null (for a regular input) or the input attribute
# path of the target of a "follows" input.
listFlakeInputs:

# A primop that, given the locked flake and the input attribute path
# of an input, fetches that input and returns an attribute set
# describing it.
fetchFlakeInput:

let
  inherit (builtins) mapAttrs foldl';

  # Construct the input denoted by the input attribute path
  # `inputAttrPath` (where `[ ]` denotes the top-level flake). This returns `edges`
  # (mapping each input name of this input to the input it denotes,
  # following "follows" indirections) and `result` (the value of this
  # input, i.e. what ends up in the `inputs` attribute of a flake).
  mkInput =
    inputAttrPath:
    let
      info = fetchFlakeInput lockedFlake inputAttrPath;

      sourceInfo =
        if info.buildTime then
          derivation {
            name = "source";
            builder = "builtin:fetch-tree";
            system = "builtin";
            __structuredAttrs = true;
            input = info.locked;
            outputHashMode = "recursive";
            outputHash = info.locked.narHash;
          }
        else
          info.sourceInfo;

      subdir = if info.buildTime then info.locked.dir or "" else info.dir;

      outPath = sourceInfo.outPath + (if subdir == "" then "" else "/" + subdir);

      flake = import (outPath + "/flake.nix");

      # Note: constructing `edges` only consults the lock data (via
      # `listFlakeInputs`), so it never causes anything to be
      # fetched. A regular input is constructed in place; a "follows"
      # input is resolved by walking the edges from the top-level
      # flake, so every distinct input is constructed (and evaluated)
      # only once.
      edges = mapAttrs (
        name: target:
        if target == null then mkInput (inputAttrPath ++ [ name ]) else getInputByAttrPath target
      ) (listFlakeInputs lockedFlake inputAttrPath);

      inputs = mapAttrs (name: input: input.result) edges;

      outputs = flake.outputs (inputs // { self = result; });

      result =
        outputs
        # We add the sourceInfo attribute for its metadata, as they are
        # relevant metadata for the flake. However, the outPath of the
        # sourceInfo does not necessarily match the outPath of the flake,
        # as the flake may be in a subdirectory of a source.
        # This is shadowed in the next //
        // sourceInfo
        // {
          # This shadows the sourceInfo.outPath
          inherit outPath;

          inherit inputs;
          inherit outputs;
          inherit sourceInfo;
          _type = "flake";
        };

    in
    {
      inherit edges;

      result =
        if info.flake then
          assert builtins.isFunction flake.outputs;
          assert !info.buildTime;
          result
        else
          sourceInfo // { inherit sourceInfo outPath; };
    };

  # Follow an input attribute path (e.g. ["dwarffs" "nixpkgs"]) from
  # the top-level flake, returning the final input.
  getInputByAttrPath = inputAttrPath: foldl' (input: name: input.edges.${name}) root inputAttrPath;

  root = mkInput [ ];

in
root.result

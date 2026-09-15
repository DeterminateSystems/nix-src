#!/usr/bin/env bash

source ./common.sh

flakeDir=$TEST_ROOT/flake
bakedDir=$TEST_ROOT/baked
cacheDir=$TEST_ROOT/binary-cache

mkdir -p "$flakeDir"
writeSimpleFlake "$flakeDir"

# Bake the flake.
nix flake bake "$flakeDir" --dest-dir "$bakedDir"
[[ -e $bakedDir/flake.nix ]]
[[ -e $bakedDir/outputs.json ]]

# The baked flake has the same outputs as the original for the current
# system. Outputs for other systems are not included by default.
nix flake show --json "path:$bakedDir" > "$TEST_ROOT/show-baked.json"
[[ $(jq -r ".inventory.packages.output.children.\"$system\".children.foo.derivation.name" < "$TEST_ROOT/show-baked.json") = simple ]]
[[ $(jq -r ".inventory.packages.output.children.\"$system\".children.default.derivation.name" < "$TEST_ROOT/show-baked.json") = simple ]]
[[ $(jq -r ".inventory.packages.output.children | has(\"someOtherSystem\")" < "$TEST_ROOT/show-baked.json") = false ]]

# The baked derivations have the same output paths as the originals.
fooPath=$(nix eval --raw "$flakeDir#foo.outPath")
[[ $(nix eval --raw "path:$bakedDir#foo.outPath") = "$fooPath" ]]
[[ $(nix eval --raw "path:$bakedDir#foo.system") = "$system" ]]
[[ $(nix eval --raw "path:$bakedDir#foo.name") = simple ]]

# The baked derivations are `builtin:substitute` derivations without inputs.
drvPath=$(nix eval --raw "path:$bakedDir#foo.drvPath")
nix derivation show "$drvPath" > "$TEST_ROOT/baked-drv.json"
[[ $(jq -r '.derivations[].builder' < "$TEST_ROOT/baked-drv.json") = builtin:substitute ]]
[[ $(jq -r '.derivations[].system' < "$TEST_ROOT/baked-drv.json") = builtin ]]
[[ $(jq -r '.derivations[].inputs.drvs | length' < "$TEST_ROOT/baked-drv.json") = 0 ]]
[[ $(jq -r '.derivations[].outputs.out.path' < "$TEST_ROOT/baked-drv.json") = "$(basename "$fooPath")" ]]

# Building a baked derivation fails if its outputs cannot be substituted.
expectStderr 1 nix build --no-link "path:$bakedDir#foo" | grepQuiet "failed to substitute"

# Build the original, copy it to a binary cache, and delete it from the store.
nix build --no-link "$flakeDir#foo"
[[ -e $fooPath/hello ]]
nix copy --to "file://$cacheDir" "$fooPath"
nix store delete "$fooPath"
[[ ! -e $fooPath ]]

# Now the baked derivation can be realised by substituting its output
# from the binary cache. (`--substitute` is needed because substitution
# is disabled automatically when there is no network access.)
nix build --no-link --substitute --substituters "file://$cacheDir" --no-require-sigs "path:$bakedDir#foo"
[[ -e $fooPath/hello ]]

# Once the output is valid, building the baked derivation succeeds even without substituters.
nix build --no-link "path:$bakedDir#foo"

# With `--all-systems`, outputs for other systems are baked as well.
nix flake bake "$flakeDir" --dest-dir "$bakedDir-all" --all-systems
nix flake show --json --all-systems "path:$bakedDir-all" > "$TEST_ROOT/show-baked-all.json"
[[ $(jq -r ".inventory.packages.output.children.someOtherSystem.children.foo.derivation.name" < "$TEST_ROOT/show-baked-all.json") = simple ]]
[[ $(nix eval --raw "path:$bakedDir-all#packages.someOtherSystem.foo.outPath") = $(nix eval --raw "$flakeDir#packages.someOtherSystem.foo.outPath") ]]

# A derivation in another flake can depend on the outputs of a baked flake.
depDir=$TEST_ROOT/dep
mkdir -p "$depDir"
cp "${config_nix}" "$depDir/"
cat > "$depDir/flake.nix" <<EOF
{
  inputs.baked.url = "path:$bakedDir";
  outputs = { self, baked }: {
    packages.$system.default = with import ./config.nix; mkDerivation {
      name = "dep";
      buildCommand = "cat \${baked.packages.$system.foo}/hello > \$out";
    };
  };
}
EOF

# The baked derivation is recorded as an input of the dependent derivation.
depDrvPath=$(nix eval --raw "$depDir#packages.$system.default.drvPath")
nix derivation show "$depDrvPath" | jq -e ".derivations[].inputs.drvs | has(\"$(basename "$drvPath")\")"

# Realising the dependent derivation fails if the baked output cannot be substituted...
nix store delete "$fooPath"
expectStderr 1 nix build --no-link "$depDir" | grepQuiet "failed to substitute"

# ... and succeeds if it can.
nix build --substitute --substituters "file://$cacheDir" --no-require-sigs "$depDir" -o "$TEST_ROOT/dep-result"
[[ $(cat "$TEST_ROOT/dep-result") = "Hello World!" ]]

# `legacyPackages` is baked too.
nix flake show --json --legacy "path:$bakedDir" > "$TEST_ROOT/show-baked-legacy.json"
[[ $(jq -r ".inventory.legacyPackages.output.children.\"$system\".children.hello.derivation.name" < "$TEST_ROOT/show-baked-legacy.json") = simple ]]

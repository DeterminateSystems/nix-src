#!/usr/bin/env bash

source ./common.sh

# Substituting from a file:// cache with --no-require-sigs requires a
# trusted user, which the NixOS tests don't provide.
TODO_NixOS

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

# `meta.mainProgram` is preserved, so `nix run` works on baked packages
# whose binary isn't named after the package.
greeterDir=$TEST_ROOT/greeter
mkdir -p "$greeterDir"
cp "${config_nix}" "$greeterDir/"
cat > "$greeterDir/flake.nix" <<EOF
{
  outputs = { self }: {
    packages.$system.greeter = with import ./config.nix; mkDerivation {
      name = "greeter";
      meta.mainProgram = "hi";
      buildCommand = ''
        mkdir -p \$out/bin
        echo '#!\${shell}' > \$out/bin/hi
        echo 'echo hello from hi' >> \$out/bin/hi
        chmod +x \$out/bin/hi
      '';
    };
  };
}
EOF

nix flake bake "$greeterDir" --dest-dir "$bakedDir-greeter"
[[ $(jq -r ".packages.output.children.\"$system\".children.greeter.derivation.mainProgram" < "$bakedDir-greeter/outputs.json") = hi ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#greeter.meta.mainProgram") = hi ]]

# Build the original so that the baked package's output is valid, then run it.
nix build --no-link "$greeterDir#greeter"
[[ $(nix run "path:$bakedDir-greeter#greeter") = "hello from hi" ]]

# Outputs whose derivation lives at a sub-path of the output attribute
# (`derivationAttrPath` in the flake schema, e.g.
# `nixosConfigurations.<name>.config.system.build.toplevel`) are baked
# at that sub-path, not at the output attribute itself.
configsDir=$TEST_ROOT/configs
mkdir -p "$configsDir"
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$configsDir/"
cat > "$configsDir/flake.nix" <<EOF
{
  outputs = { self }: {
    nixosConfigurations.foo = {
      config.system.build.toplevel = import ./simple.nix;
      pkgs.stdenv.system = "$system";
    };
    homeConfigurations.bar.activationPackage = import ./simple.nix;
  };
}
EOF

# The schemas recognise these outputs in the original flake.
nix flake show --json "$configsDir" > "$TEST_ROOT/show-configs.json"
[[ $(jq -r '.inventory.nixosConfigurations.output.children.foo.derivation.name' < "$TEST_ROOT/show-configs.json") = simple ]]
[[ $(jq -r '.inventory.homeConfigurations.output.children.bar.derivation.name' < "$TEST_ROOT/show-configs.json") = simple ]]

# The inventory written by `nix flake bake` records the derivation attribute path.
nix flake bake "$configsDir" --dest-dir "$bakedDir-configs"
[[ $(jq -r '.nixosConfigurations.output.children.foo.derivation.name' < "$bakedDir-configs/outputs.json") = simple ]]
[[ $(jq -c '.nixosConfigurations.output.children.foo.derivationAttrPath' < "$bakedDir-configs/outputs.json") = '["config","system","build","toplevel"]' ]]
[[ $(jq -c '.homeConfigurations.output.children.bar.derivationAttrPath' < "$bakedDir-configs/outputs.json") = '["activationPackage"]' ]]

# The baked derivations live at the nested path and have the same
# output paths as the originals...
toplevelPath=$(nix eval --raw "$configsDir#nixosConfigurations.foo.config.system.build.toplevel.outPath")
[[ $(nix eval --raw "path:$bakedDir-configs#nixosConfigurations.foo.config.system.build.toplevel.outPath") = "$toplevelPath" ]]
[[ $(nix eval --raw "path:$bakedDir-configs#nixosConfigurations.foo.config.system.build.toplevel.system") = "$system" ]]
activationPath=$(nix eval --raw "$configsDir#homeConfigurations.bar.activationPackage.outPath")
[[ $(nix eval --raw "path:$bakedDir-configs#homeConfigurations.bar.activationPackage.outPath") = "$activationPath" ]]

# ... and not at the output attribute itself.
[[ $(nix eval "path:$bakedDir-configs#nixosConfigurations.foo" --apply 'x: x ? drvPath') = false ]]
[[ $(nix eval "path:$bakedDir-configs#homeConfigurations.bar" --apply 'x: x ? drvPath') = false ]]

# `nix flake show` on a baked flake evaluates the flake schemas against
# the baked values. This works for `homeConfigurations`, whose schema
# only needs `activationPackage.system`. It does not currently work for
# `nixosConfigurations`, whose schema reads `pkgs.stdenv.system`, which
# the baked flake does not provide, so we don't test that here.
homeDir=$TEST_ROOT/home
mkdir -p "$homeDir"
cp ../simple.nix ../simple.builder.sh "${config_nix}" "$homeDir/"
cat > "$homeDir/flake.nix" <<EOF
{
  outputs = { self }: {
    homeConfigurations.bar.activationPackage = import ./simple.nix;
  };
}
EOF
nix flake bake "$homeDir" --dest-dir "$bakedDir-home"
nix flake show --json "path:$bakedDir-home" > "$TEST_ROOT/show-baked-home.json"
[[ $(jq -r '.inventory.homeConfigurations.output.children.bar.derivation.name' < "$TEST_ROOT/show-baked-home.json") = simple ]]
[[ $(jq -c '.inventory.homeConfigurations.output.children.bar.forSystems' < "$TEST_ROOT/show-baked-home.json") = "[\"$system\"]" ]]

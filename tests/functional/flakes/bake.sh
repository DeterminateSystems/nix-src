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

# Baking requires the `baked-derivation` experimental feature.
expectStderr 1 nix flake bake "$flakeDir" --dest-dir "$bakedDir" | grepQuiet "experimental Nix feature 'baked-derivation' is disabled"
enableFeatures "baked-derivation"

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

# A `builtin:substitute` derivation must have input-addressed outputs with known paths.
jq '.derivations[]' < "$TEST_ROOT/baked-drv.json" > "$TEST_ROOT/baked-drv-single.json"
[[ $(nix derivation add < "$TEST_ROOT/baked-drv-single.json") = "$drvPath" ]]
jq '.outputs.out = {}' < "$TEST_ROOT/baked-drv-single.json" | expectStderr 1 nix derivation add | grepQuiet "must have input-addressed outputs"
jq '.outputs.out = {"method": "nar", "hash": "sha256-iUUXyRY8iW7DGirb0zwGgf1fRbLA7wimTJKgP7l/OQ8="}' < "$TEST_ROOT/baked-drv-single.json" | expectStderr 1 nix derivation add | grepQuiet "must have input-addressed outputs"

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
enableFeatures "ca-derivations"
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
    packages.$system.multi = with import ./config.nix; mkDerivation {
      name = "multi";
      outputs = [ "bin" "dev" ];
      buildCommand = "mkdir \$bin \$dev; echo bin > \$bin/what; echo dev > \$dev/what";
    };
    packages.$system.multiDev = self.packages.$system.multi.dev;
    # Content-addressed derivations cannot be baked.
    packages.$system.ca = with import ./config.nix; mkDerivation {
      name = "ca";
      __contentAddressed = true;
      outputHashMode = "recursive";
      outputHashAlgo = "sha256";
      buildCommand = "echo ca > \$out";
    };
  };
}
EOF

# Derivations that cannot be baked produce a warning and are omitted.
nix flake bake "$greeterDir" --dest-dir "$bakedDir-greeter" 2>&1 | grepQuiet "warning: cannot bake 'packages.$system.ca'"
[[ $(nix eval "path:$bakedDir-greeter#packages.$system" --apply 'x: x ? ca') = false ]]
[[ $(nix eval "path:$bakedDir-greeter#packages.$system" --apply 'x: x ? greeter') = true ]]
[[ $(jq -r ".packages.output.children.\"$system\".children.greeter.derivation.mainProgram" < "$bakedDir-greeter/outputs.json") = hi ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#greeter.meta.mainProgram") = hi ]]

# The system of each derivation is recorded, so the baked flake doesn't depend on the schema's `forSystems`.
[[ $(jq -r ".packages.output.children.\"$system\".children.greeter.derivation.system" < "$bakedDir-greeter/outputs.json") = "$system" ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#greeter.system") = "$system" ]]

# Build the original so that the baked package's output is valid, then run it.
nix build --no-link "$greeterDir#greeter"
[[ $(nix run "path:$bakedDir-greeter#greeter") = "hello from hi" ]]

# The default output isn't necessarily `out`: multi-output derivations
# and attributes that refer to a specific output are baked faithfully.
[[ $(jq -r ".packages.output.children.\"$system\".children.multi.derivation.outputName" < "$bakedDir-greeter/outputs.json") = bin ]]
[[ $(jq -r ".packages.output.children.\"$system\".children.multiDev.derivation.outputName" < "$bakedDir-greeter/outputs.json") = dev ]]
multiBin=$(nix eval --raw "$greeterDir#multi.outPath")
multiDev=$(nix eval --raw "$greeterDir#multi.dev.outPath")
[[ $multiBin != "$multiDev" ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#multi.outputName") = bin ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#multi.outPath") = "$multiBin" ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#multi.bin.outPath") = "$multiBin" ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#multi.dev.outPath") = "$multiDev" ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#multi.dev.outputName") = dev ]]
[[ $(nix eval --json "path:$bakedDir-greeter#multi.outputs") = '["bin","dev"]' ]]
[[ $(nix eval "path:$bakedDir-greeter#multi" --apply 'x: builtins.length x.all') = 2 ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#multiDev.outputName") = dev ]]
[[ $(nix eval --raw "path:$bakedDir-greeter#multiDev.outPath") = "$multiDev" ]]

# Building a specific output of a baked derivation works.
nix build --no-link "$greeterDir#multi^*"
[[ $(nix build --no-link --json "path:$bakedDir-greeter#multi^dev" | jq -r '.[0].outputs | keys | join(",")') = dev ]]
[[ $(nix build --no-link --json "path:$bakedDir-greeter#multi^dev" | jq -r '.[0].outputs.dev') = "$multiDev" ]]
[[ $(cat "$multiDev/what") = dev ]]

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

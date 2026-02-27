#!/usr/bin/env bash

experimental_features="provenance"

source common.sh

TODO_NixOS

createFlake1

outPath=$(nix build --print-out-paths --no-link "$flake1Dir#packages.$system.default")
drvPath=$(nix eval --raw "$flake1Dir#packages.$system.default.drvPath")
rev=$(nix flake metadata --json "$flake1Dir" | jq -r .locked.rev)
lastModified=$(nix flake metadata --json "$flake1Dir" | jq -r .locked.lastModified)
treePath=$(nix flake prefetch --json "$flake1Dir" | jq -r .storePath)
builder=$(nix eval --raw "$flake1Dir#packages.$system.default._builder")

# Building a derivation should have tree+subpath+flake+meta+build provenance.
[[ "$(nix path-info --json --json-format 1 "$outPath" | jq ".\"$outPath\".provenance")" == "$(cat <<EOF
{
  "buildHost": "test-host",
  "drv": "$(basename "$drvPath")",
  "next": {
    "meta": {
      "license": [
        {
          "deprecated": true,
          "free": true,
          "fullName": "GNU Lesser General Public License v2.1",
          "redistributable": true,
          "shortName": "lgpl21",
          "spdxId": "LGPL-2.1",
          "url": "https://spdx.org/licenses/LGPL-2.1.html"
        }
      ]
    },
    "next": {
      "flakeOutput": "packages.$system.default",
      "next": {
        "next": {
          "attrs": {
            "lastModified": $lastModified,
            "ref": "refs/heads/master",
            "rev": "$rev",
            "revCount": 1,
            "type": "git",
            "url": "file://$flake1Dir"
          },
          "type": "tree"
        },
        "subpath": "/flake.nix",
        "type": "subpath"
      },
      "pure": true,
      "type": "flake"
    },
    "type": "derivation"
  },
  "output": "out",
  "system": "$system",
  "type": "build"
}
EOF
)" ]]

# Flakes should have "tree" provenance.
[[ $(nix path-info --json --json-format 1 "$treePath" | jq ".\"$treePath\".provenance") = $(cat <<EOF
{
  "attrs": {
    "lastModified": $lastModified,
    "ref": "refs/heads/master",
    "rev": "$rev",
    "revCount": 1,
    "type": "git",
    "url": "file://$flake1Dir"
  },
  "type": "tree"
}
EOF
) ]]

# A source file should have tree+subpath provenance.
[[ $(nix path-info --json --json-format 1 "$builder" | jq ".\"$builder\".provenance") = $(cat <<EOF
{
  "next": {
    "attrs": {
      "lastModified": $lastModified,
      "ref": "refs/heads/master",
      "rev": "$rev",
      "revCount": 1,
      "type": "git",
      "url": "file://$flake1Dir"
    },
    "type": "tree"
  },
  "subpath": "/simple.builder.sh",
  "type": "subpath"
}
EOF
) ]]

[[ "$(nix provenance show "$builder")" = $(cat <<EOF
[1m$builder[0m
← from file [1m/simple.builder.sh[0m
← from tree [1mgit+file://$flake1Dir?ref=refs/heads/master&rev=$rev[0m
EOF
) ]]

# Verify the provenance of all store paths.
nix provenance verify --all

# Verification should fail if the sources cannot be fetched
mv "$flake1Dir" "$flake1Dir-tmp"
expectStderr 1 nix provenance verify --all | grepQuiet "Git repository.*does not exist"
mv "$flake1Dir-tmp" "$flake1Dir"

# Check that substituting from a binary cache adds "copied" provenance.
binaryCache="$TEST_ROOT/binary-cache"
nix copy --to "file://$binaryCache" "$outPath"

clearStore

export _NIX_FORCE_HTTP=1 # force use of the NAR info disk cache

# Check that provenance is cached correctly.
[[ $(nix path-info --json --json-format 1 --store "file://$binaryCache" "$outPath" | jq ".\"$outPath\".provenance") != null ]]
[[ $(nix path-info --json --json-format 1 --store "file://$binaryCache" "$outPath" | jq ".\"$outPath\".provenance") != null ]]

nix copy --from "file://$binaryCache" "$outPath" --no-check-sigs

[[ "$(nix path-info --json --json-format 1 "$outPath" | jq ".\"$outPath\".provenance")" = "$(cat <<EOF
{
  "from": "file://$binaryCache",
  "next": {
    "buildHost": "test-host",
    "drv": "$(basename "$drvPath")",
    "next": {
      "meta": {
        "license": [
          {
            "deprecated": true,
            "free": true,
            "fullName": "GNU Lesser General Public License v2.1",
            "redistributable": true,
            "shortName": "lgpl21",
            "spdxId": "LGPL-2.1",
            "url": "https://spdx.org/licenses/LGPL-2.1.html"
          }
        ]
      },
      "next": {
        "flakeOutput": "packages.$system.default",
        "next": {
          "next": {
            "attrs": {
              "lastModified": $lastModified,
              "ref": "refs/heads/master",
              "rev": "$rev",
              "revCount": 1,
              "type": "git",
              "url": "file://$flake1Dir"
            },
            "type": "tree"
          },
          "subpath": "/flake.nix",
          "type": "subpath"
        },
        "pure": true,
        "type": "flake"
      },
      "type": "derivation"
    },
    "output": "out",
    "system": "$system",
    "type": "build"
  },
  "type": "copied"
}
EOF
)" ]]

unset _NIX_FORCE_HTTP

# Test `nix provenance show`.
[[ "$(nix provenance show "$outPath")" = "$(cat <<EOF
[1m$outPath[0m
← copied from [1mfile://$binaryCache[0m
← built from derivation [1m$drvPath[0m (output [1mout[0m) on [1mtest-host[0m for [1m$system[0m
← with derivation metadata
  {
    "license": [
      {
        "deprecated": true,
        "free": true,
        "fullName": "GNU Lesser General Public License v2.1",
        "redistributable": true,
        "shortName": "lgpl21",
        "spdxId": "LGPL-2.1",
        "url": "https://spdx.org/licenses/LGPL-2.1.html"
      }
    ]
  }
← instantiated from flake output [1mgit+file://$flake1Dir?ref=refs/heads/master&rev=$rev#packages.$system.default[0m
EOF
)" ]]

nix provenance verify --all

# Check that --impure does not add additional provenance.
clearStore
nix build --impure --print-out-paths --no-link "$flake1Dir#packages.$system.default"
[[ "$(nix path-info --json --json-format 1 "$drvPath" | jq ".\"$drvPath\".provenance")" = "$(cat << EOF
{
  "meta": {
    "license": [
      {
        "deprecated": true,
        "free": true,
        "fullName": "GNU Lesser General Public License v2.1",
        "redistributable": true,
        "shortName": "lgpl21",
        "spdxId": "LGPL-2.1",
        "url": "https://spdx.org/licenses/LGPL-2.1.html"
      }
    ]
  },
  "next": {
    "flakeOutput": "packages.$system.default",
    "next": {
      "next": {
        "attrs": {
          "lastModified": $lastModified,
          "ref": "refs/heads/master",
          "rev": "$rev",
          "revCount": 1,
          "type": "git",
          "url": "file://$flake1Dir"
        },
        "type": "tree"
      },
      "subpath": "/flake.nix",
      "type": "subpath"
    },
    "pure": false,
    "type": "flake"
  },
  "type": "derivation"
}
EOF
)" ]]

clearStore
echo foo > "$flake1Dir/somefile"
git -C "$flake1Dir" add somefile
nix build --impure --print-out-paths --no-link "$flake1Dir#packages.$system.default"
[[ $(nix path-info --json --json-format 1 "$builder" | jq ".\"$builder\".provenance") != null ]]

[[ "$(nix provenance show "$outPath")" = "$(cat <<EOF
[1m$outPath[0m
← built from derivation [1m$drvPath[0m (output [1mout[0m) on [1mtest-host[0m for [1m$system[0m
← with derivation metadata
  {
    "license": [
      {
        "deprecated": true,
        "free": true,
        "fullName": "GNU Lesser General Public License v2.1",
        "redistributable": true,
        "shortName": "lgpl21",
        "spdxId": "LGPL-2.1",
        "url": "https://spdx.org/licenses/LGPL-2.1.html"
      }
    ]
  }
← [31;1mimpurely[0m instantiated from [31;1munlocked[0m flake output [1mgit+file://$flake1Dir#packages.$system.default[0m
EOF
)" ]]

[[ "$(nix provenance show "$builder")" = $(cat <<EOF
[1m$builder[0m
← from file [1m/simple.builder.sh[0m
← from [31;1munlocked[0m tree [1mgit+file://$flake1Dir[0m
EOF
) ]]

nix provenance verify --all

# Test that impure builds fail verification.
clearStore
echo x > "$TEST_ROOT/counter"
cat > "$flake1Dir/flake.nix" <<EOF
{
  outputs = inputs: rec {
    packages.$system = rec {
      default =
        with import ./config.nix;
        mkDerivation {
          name = "simple";
          buildCommand = ''
            set -x
            cat "$TEST_ROOT/counter" > \$out
            echo x >> "$TEST_ROOT/counter"
          '';
        };
    };
  };
}
EOF
outPath=$(nix build --print-out-paths --no-link "$flake1Dir")

expectStderr 1 nix provenance verify --all | grepQuiet "derivation .* may not be deterministic: output .* differs"

# Test various types of source files.
clearStore
echo x > "$TEST_ROOT/counter"
cat > "$flake1Dir/flake.nix" <<EOF
{
  outputs = { self }: rec {
    packages.$system = rec {
      default =
        with import ./config.nix;
        mkDerivation {
          name = "simple";
          buildCommand = "mkdir \$out";
          src1 = ./config.nix;
          src2 = self;
          src3 = ./.;
          src4 = builtins.path { name = "foo"; path = ./.; filter = path: type: builtins.match ".*\.nix" path != null; };
        };
    };
  };
}
EOF
outPath=$(nix build --print-out-paths --no-link "$flake1Dir")

nix provenance verify --all

# Test fetchurl provenance.
clearStore

echo hello > "$TEST_ROOT/hello.txt"

path="$(nix store prefetch-file --json "file://$TEST_ROOT/hello.txt" | jq -r .storePath)"

[[ "$(nix provenance show "$path")" = $(cat <<EOF
[1m$path[0m
← fetched from URL [1mfile://$TEST_ROOT/hello.txt[0m
EOF
) ]]

nix provenance verify "$path"

echo barf > "$TEST_ROOT/hello.txt"

expectStderr 1 nix provenance verify "$path" | grepQuiet "hash mismatch for URL"

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
      "type": "flake"
    },
    "type": "meta"
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

# Check that substituting from a binary cache adds "copied" provenance.
binaryCache="$TEST_ROOT/binary-cache"
nix copy --to "file://$binaryCache" "$outPath"

clearStore

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
        "type": "flake"
      },
      "type": "meta"
    },
    "output": "out",
    "system": "$system",
    "type": "build"
  },
  "type": "copied"
}
EOF
)" ]]

# Test `nix provenance show`.
[[ "$(nix provenance show "$outPath")" = $(cat <<EOF
[1m$outPath[0m
← copied from [1mfile://$binaryCache[0m
← built from derivation [1m$drvPath[0m (output [1mout[0m) on [1mtest-host[0m for [1m$system[0m
← with metadata
← instantiated from flake output [1mgit+file://$flake1Dir?ref=refs/heads/master&rev=$rev#packages.$system.default[0m
EOF
) ]]

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
  "next": null,
  "type": "meta"
}
EOF
)" ]]

clearStore
echo foo > "$flake1Dir/somefile"
git -C "$flake1Dir" add somefile
nix build --impure --print-out-paths --no-link "$flake1Dir#packages.$system.default"
builder=$(nix eval --raw "$flake1Dir#packages.$system.default._builder")
[[ $(nix path-info --json --json-format 1 "$builder" | jq ".\"$builder\".provenance") = null ]]

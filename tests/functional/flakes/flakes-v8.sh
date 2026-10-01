#!/usr/bin/env bash

# Run the flake tests with version 8 lock files.

export NIX_CONFIG="extra-experimental-features = lock-file-v8
lock-file-format = 8"

# shellcheck source=/dev/null
source ./flakes.sh
